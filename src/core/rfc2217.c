#include "rfc2217.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "lwip/sockets.h"

#include "platform.h"
#include "clients.h"
#include "console.h"
#include "rfc2217_proto.h"

#define RFC2217_PORT          2217
#define RFC2217_TASK_STACK    1024
#define RFC2217_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define MAX_CLIENTS           CLIENTS_MAX // slots; the shared budget (clients.h) caps pages + RFC 2217
#define TICK_MS               20
#define CLIENT_LINE_MAX       256
#define LINE_IDLE_MS          50  // a line without "\n" (just "\r") goes out after this pause
#define KEEPALIVE_IDLE_S      30  // TCP keepalive (see add_client)
#define KEEPALIVE_INTERVAL_S  5
#define KEEPALIVE_COUNT       3

typedef struct {
    int fd;                        // -1: free
    char ip[16];
    uint32_t since_ms;             // connected at (the oldest is replaced when all are taken)
    rfc2217_t proto;
    uint32_t seq;                  // position in the console's routed lines (console_read_line)
    int modem_sent;                // modem state last announced (-1: not yet)
    // Lines go to the RT4K whole: bytes trickling in separately could be cut apart by one of
    // Cruller's own transfers (its "\r<cmd>\r\n" would end the client's line) or another client's.
    uint8_t line[CLIENT_LINE_MAX];
    size_t line_len;
    uint32_t line_last_ms;
} client_t;

static client_t clients[MAX_CLIENTS];

static uint32_t now_ms(void) {
    return plat_ms();
}

static bool send_all(int fd, const uint8_t *p, size_t len) {
    while (len) {
        const int n = send(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

// A whole line from the client, as a console command of its own (its replies come back to it only).
static void flush_line(client_t *c) {
    size_t start = 0, end = c->line_len;
    while (start < end && (c->line[start] == '\r' || c->line[start] == '\n')) start++;
    while (end > start && (c->line[end - 1] == '\r' || c->line[end - 1] == '\n')) end--;
    if (end > start) {
        c->line[end] = 0;
        if (!console_send(CON_CLIENT((int)(c - clients)), (const char *)c->line + start)) {
            printf("rfc2217: client %s: command dropped (console queue full)\n", c->ip);
        }
    }
    c->line_len = 0;
}

static void from_client(client_t *c, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        c->line[c->line_len++] = data[i];
        if (data[i] == '\n' || c->line_len == sizeof(c->line) - 1) flush_line(c); // room for the NUL
    }
    c->line_last_ms = now_ms();
}

static void drop_client(client_t *c, const char *why) {
    if (c->fd < 0) return;
    closesocket(c->fd);
    c->fd = -1;
    c->line_len = 0;
    clients_give();
    printf("rfc2217: client %s %s\n", c->ip, why);
    c->ip[0] = 0;
}

static void add_client(int fd, const struct sockaddr_in *peer, uint8_t *buf, size_t size) {
    if (!clients_take()) {
        // The shared budget is full: the oldest RFC 2217 client makes room (a crashed one can't lock
        // the others out), but a web page is never replaced for it.
        client_t *oldest = NULL;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0 && (!oldest || clients[i].since_ms - oldest->since_ms > 0x80000000u)) oldest = &clients[i];
        }
        if (!oldest) {
            printf("rfc2217: no room: all %d client slots are taken by web pages\n", CLIENTS_MAX);
            closesocket(fd);
            return;
        }
        drop_client(oldest, "replaced by a new connection");
        clients_take(); // the slot it gave back
    }
    client_t *c = NULL;
    for (int i = 0; i < MAX_CLIENTS && !c; i++) if (clients[i].fd < 0) c = &clients[i];
    const int one = 1;
    const struct timeval snd = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // A client that vanished without closing (power cut, network gone) would keep its slot: TCP
    // keepalive probes a quiet connection after KEEPALIVE_IDLE_S, then every KEEPALIVE_INTERVAL_S,
    // and gives up after KEEPALIVE_COUNT unanswered probes (~45 s). Clients' TCP stacks answer on their
    // own; RFC 2217 has nothing for it (Telnet's "Are You There" goes unanswered by pyserial).
    const int idle = KEEPALIVE_IDLE_S, interval = KEEPALIVE_INTERVAL_S, count = KEEPALIVE_COUNT;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    c->fd = fd;
    c->since_ms = now_ms();
    inet_ntoa_r(peer->sin_addr, c->ip, sizeof(c->ip));
    rfc2217_init(&c->proto);
    c->seq = console_head(); // from now on, not what came before
    c->modem_sent = -1;
    c->line_len = 0;
    const size_t n = rfc2217_greeting(&c->proto, buf, size);
    if (!send_all(fd, buf, n)) drop_client(c, "failed");
    else printf("rfc2217: client %s connected\n", c->ip);
}

static void rfc2217_task(void *param) {
    (void)param;
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
    const int server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    const int one = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(RFC2217_PORT), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 2) < 0) {
        printf("rfc2217: cannot listen on port %d\n", RFC2217_PORT);
        vTaskDelete(NULL);
    }
    printf("rfc2217: listening on port %d (up to %d clients)\n", RFC2217_PORT, MAX_CLIENTS);
    static uint8_t in[512], data[512], reply[256], text[256], out[520];
    for (;;) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(server, &rd);
        int maxfd = server;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) continue;
            FD_SET(clients[i].fd, &rd);
            if (clients[i].fd > maxfd) maxfd = clients[i].fd;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = TICK_MS * 1000};
        const int ready = select(maxfd + 1, &rd, NULL, NULL, &tv);

        if (ready > 0 && FD_ISSET(server, &rd)) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            const int fd = accept(server, (struct sockaddr *)&peer, &plen);
            if (fd >= 0) add_client(fd, &peer, out, sizeof(out));
        }

        const uint8_t modem = (uint8_t)(rt4k_modem_status() & 0xf0);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &clients[i];
            if (c->fd < 0) continue;
            if (ready > 0 && FD_ISSET(c->fd, &rd)) {
                const int n = recv(c->fd, in, sizeof(in), 0);
                if (n <= 0) {
                    drop_client(c, "left");
                    continue;
                }
                rfc2217_io_t io = {.data = data, .data_max = sizeof(data), .reply = reply, .reply_max = sizeof(reply)};
                rfc2217_input(&c->proto, in, (size_t)n, &io, modem);
                if (io.reply_len && !send_all(c->fd, reply, io.reply_len)) {
                    drop_client(c, "failed");
                    continue;
                }
                if (io.data_len) from_client(c, data, io.data_len);
            }
            if (c->line_len && now_ms() - c->line_last_ms >= LINE_IDLE_MS) flush_line(c);

            // The modem state (CTS, DSR...), announced at connect and on every change.
            if (modem != c->modem_sent) {
                const size_t n = rfc2217_modemstate(modem, out, sizeof(out));
                if (!send_all(c->fd, out, n)) {
                    drop_client(c, "failed");
                    continue;
                }
                c->modem_sent = modem;
            }

            // What the RT4K says: the replies to this client's commands, and lines outside any
            // command's reply window (see console.h). The RT4K ends its lines with "\n".
            int owner;
            while (c->fd >= 0 && console_read_line(&c->seq, &owner, (char *)text, sizeof(text) - 1)) {
                if (owner != CON_CLIENT(i) && owner != CON_BROADCAST) continue;
                size_t n = strlen((char *)text);
                text[n++] = '\n';
                for (size_t off = 0; off < n;) {
                    size_t used;
                    const size_t m = rfc2217_escape(text + off, n - off, out, sizeof(out), &used);
                    if (!send_all(c->fd, out, m)) {
                        drop_client(c, "failed");
                        break;
                    }
                    off += used;
                }
            }
        }
    }
}

int rfc2217_count(int *max) {
    if (max) *max = MAX_CLIENTS;
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) n += clients[i].fd >= 0;
    return n;
}

void rfc2217_clients(char *out, size_t size) {
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < MAX_CLIENTS && o < size; i++) {
        if (clients[i].fd < 0) continue;
        o += (size_t)snprintf(out + o, size - o, "%s%s", o ? " " : "", clients[i].ip);
    }
}

void rfc2217_start(void) {
    xTaskCreate(rfc2217_task, "rfc2217", PLAT_STACK(RFC2217_TASK_STACK), NULL, RFC2217_TASK_PRIORITY, NULL);
}
