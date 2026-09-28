#include "rfc2217.h"

#include <stdio.h>
#include <string.h>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#include "rfc2217_proto.h"
#include "svs_usb.h"

#define RFC2217_PORT          2217
#define RFC2217_TASK_STACK    4096
#define RFC2217_TASK_PRIORITY 4
#define MAX_CLIENTS           3
#define TICK_MS               20
#define CLIENT_LINE_MAX       128
#define LINE_IDLE_MS          50  // a line without "\n" (just "\r") goes out after this pause
#define KEEPALIVE_IDLE_S      30  // TCP keepalive (see add_client)
#define KEEPALIVE_INTERVAL_S  5
#define KEEPALIVE_COUNT       3

// RFC 2217 modem state bits (0x10 CTS, 0x20 DSR, 0x40 RI, 0x80 CD): a live line while the SVS is
// plugged in, all dropped when it is not.
#define MODEM_LINK_UP   0xb0
#define MODEM_LINK_DOWN 0x00

struct client_t {
    int fd;                        // -1: free
    char ip[16];
    int64_t since_ms;              // connected at (the oldest is replaced when all are taken)
    rfc2217_t proto;
    uint32_t seq;                  // last traffic-log entry sent to it (svs_usb::log_since)
    int modem_sent;                // modem state last announced (-1: not yet)
    // Lines go to the SVS whole, as the console does: bytes trickling in separately would be sent as
    // several commands.
    uint8_t line[CLIENT_LINE_MAX];
    size_t line_len;
    int64_t line_last_ms;
};

static client_t clients[MAX_CLIENTS];

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
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

// Text for the client, with 0xFF doubled.
static bool send_text(client_t *c, const std::string &text) {
    uint8_t out[128];
    const uint8_t *p = (const uint8_t *)text.data();
    size_t left = text.size();
    while (left) {
        size_t used;
        const size_t m = rfc2217_escape(p, left, out, sizeof(out), &used);
        if (!send_all(c->fd, out, m)) return false;
        p += used;
        left -= used;
    }
    return true;
}

static void drop_client(client_t *c, const char *why) {
    if (c->fd < 0) return;
    closesocket(c->fd);
    c->fd = -1;
    c->line_len = 0;
    printf("rfc2217: client %s %s\n", c->ip, why);
    c->ip[0] = 0;
}

// A whole line from the client, sent to the SVS like a command from the web UI.
static void flush_line(client_t *c) {
    size_t start = 0, end = c->line_len;
    while (start < end && (c->line[start] == '\r' || c->line[start] == '\n')) start++;
    while (end > start && (c->line[end - 1] == '\r' || c->line[end - 1] == '\n')) end--;
    c->line_len = 0;
    if (end == start) return;
    const std::string cmd((const char *)c->line + start, end - start);
    const esp_err_t err = svs_usb::send(cmd);
    const char *why = err == ESP_OK                  ? nullptr
                      : err == ESP_ERR_INVALID_STATE ? "SVS not connected"
                      : err == ESP_ERR_NOT_SUPPORTED ? "listen-only mode"
                      : err == ESP_ERR_NOT_ALLOWED   ? "SVS firmware update in progress"
                                                     : "send error";
    if (why && !send_text(c, std::string("(") + why + ", not sent: " + cmd + ")\r\n")) drop_client(c, "failed");
}

static void from_client(client_t *c, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len && c->fd >= 0; i++) {
        c->line[c->line_len++] = data[i];
        if (data[i] == '\n' || c->line_len == sizeof(c->line)) flush_line(c);
    }
    c->line_last_ms = now_ms();
}

static void add_client(int fd, const struct sockaddr_in *peer, uint8_t *buf, size_t size) {
    client_t *c = nullptr;
    for (int i = 0; i < MAX_CLIENTS && !c; i++) if (clients[i].fd < 0) c = &clients[i];
    if (!c) {
        // All taken: the oldest makes room (a crashed client can't lock the others out).
        c = &clients[0];
        for (int i = 1; i < MAX_CLIENTS; i++) if (clients[i].since_ms < c->since_ms) c = &clients[i];
        drop_client(c, "replaced by a new connection");
    }
    const int one = 1;
    const struct timeval snd = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // A client that vanished without closing (power cut, network gone) would keep its slot: TCP
    // keepalive probes a quiet connection after KEEPALIVE_IDLE_S, then every KEEPALIVE_INTERVAL_S,
    // and gives up after KEEPALIVE_COUNT unanswered probes (~45 s).
    const int idle = KEEPALIVE_IDLE_S, interval = KEEPALIVE_INTERVAL_S, count = KEEPALIVE_COUNT;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    c->fd = fd;
    c->since_ms = now_ms();
    inet_ntoa_r(peer->sin_addr, c->ip, sizeof(c->ip));
    rfc2217_init(&c->proto);
    c->seq = svs_usb::log_head();  // from now on, not what came before
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
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RFC2217_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (server < 0 || bind(server, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 2) < 0) {
        printf("rfc2217: cannot listen on port %d\n", RFC2217_PORT);
        vTaskDelete(NULL);
    }
    printf("rfc2217: listening on port %d (up to %d clients)\n", RFC2217_PORT, MAX_CLIENTS);
    static uint8_t in[256], data[256], reply[256], out[128];
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
        struct timeval tv = {};
        tv.tv_usec = TICK_MS * 1000;
        const int ready = select(maxfd + 1, &rd, NULL, NULL, &tv);

        if (ready > 0 && FD_ISSET(server, &rd)) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            const int fd = accept(server, (struct sockaddr *)&peer, &plen);
            if (fd >= 0) add_client(fd, &peer, out, sizeof(out));
        }

        const uint8_t modem = svs_usb::is_connected() ? MODEM_LINK_UP : MODEM_LINK_DOWN;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client_t *c = &clients[i];
            if (c->fd < 0) continue;
            if (ready > 0 && FD_ISSET(c->fd, &rd)) {
                const int n = recv(c->fd, in, sizeof(in), 0);
                if (n <= 0) {
                    drop_client(c, "left");
                    continue;
                }
                rfc2217_io_t io = {};
                io.data = data;
                io.data_max = sizeof(data);
                io.reply = reply;
                io.reply_max = sizeof(reply);
                rfc2217_input(&c->proto, in, (size_t)n, &io, modem);
                if (io.reply_len && !send_all(c->fd, reply, io.reply_len)) {
                    drop_client(c, "failed");
                    continue;
                }
                if (io.data_len) from_client(c, data, io.data_len);
                if (c->fd < 0) continue;
            }
            if (c->line_len && now_ms() - c->line_last_ms >= LINE_IDLE_MS) flush_line(c);
            if (c->fd < 0) continue;

            // The modem state, announced at connect and on every change.
            if (modem != c->modem_sent) {
                const size_t n = rfc2217_modemstate(modem, out, sizeof(out));
                if (!send_all(c->fd, out, n)) {
                    drop_client(c, "failed");
                    continue;
                }
                c->modem_sent = modem;
            }

            // What the SVS says (the web UI's traffic log, '<' entries), one line each.
            for (const auto &e : svs_usb::log_since(c->seq, 16)) {
                c->seq = e.seq;
                if (e.dir != '<') continue;
                if (!send_text(c, e.text + "\r\n")) {
                    drop_client(c, "failed");
                    break;
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
    xTaskCreate(rfc2217_task, "rfc2217", RFC2217_TASK_STACK, NULL, RFC2217_TASK_PRIORITY, NULL);
}
