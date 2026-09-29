#include "rfc2217.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#include "rfc2217_proto.h"
#include "svs_settings.h"
#include "svs_usb.h"

#define RFC2217_PORT          2217
#define RFC2217_TASK_STACK    4096
#define RFC2217_TASK_PRIORITY 4
#define MAX_CLIENTS           3
#define TICK_MS               20
#define TRACE_MS              60000 // how long after connecting what the SVS says is also noted in the log
#define TRACE_AFTER_TX_MS     3000  // ... and how long after a client sends something
#define RESTART_MIN_GAP_MS    4000  // a client that reopens its port in a loop can't keep the SVS restarting
#define ECHO_WAIT_MS          1000  // how long a Y/G answer is waited for before what is held goes out
#define CLIENT_LINE_MAX       128
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
    uint16_t port;
    int64_t since_ms;              // connected at (the oldest is replaced when all are taken)
    int64_t last_rx_ms;            // last time it sent anything
    uint32_t rx, tx;               // bytes received from / sent to it
    uint32_t commands, refused;    // lines sent to the SVS / not sent (SVS not connected, ...)
    rfc2217_t proto;
    uint32_t rx_pos;               // how far into what the SVS sent (svs_usb::rx_since) it has been given
    int modem_sent;                // modem state last announced (-1: not yet)
    // What it types, for the traffic log only (the bytes themselves go to the SVS as they come)
    uint8_t line[CLIENT_LINE_MAX];
    size_t line_len;
    rfc2217_echo_t echo;           // drops the input number the SVS puts before the value of Y<n> / G<n>
    int64_t echo_ms;               // when that was asked
};

static client_t clients[MAX_CLIENTS];

static int64_t now_ms(void) {
    return esp_timer_get_time() / 1000;
}

// Telnet/RFC 2217 bytes in words ("WILL BINARY, SET-BAUDRATE 9600, text \"AT\""), for the traffic log
static std::string describe(const uint8_t *p, size_t len) {
    char out[384];
    rfc2217_describe(p, len, out, sizeof(out));
    return out;
}

// A note in the web UI's traffic log (not forwarded to the clients, which only get what the SVS says)
static void note(const client_t *c, const std::string &text) {
    svs_usb::log_note(std::string("RFC2217 ") + c->ip + ": " + text);
}

static bool send_all(int fd, const uint8_t *p, size_t len);

// send_all() to a client, counted
static bool send_to(client_t *c, const uint8_t *p, size_t len) {
    c->tx += (uint32_t)len;
    return send_all(c->fd, p, len);
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
        if (!send_to(c, out, m)) return false;
        p += used;
        left -= used;
    }
    return true;
}

static int connected_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) n += clients[i].fd >= 0;
    return n;
}

// "3 min 20 s" for the log
static std::string span(int64_t ms) {
    const int64_t s = ms / 1000;
    char b[32];
    if (s < 60) snprintf(b, sizeof(b), "%d s", (int)s);
    else if (s < 3600) snprintf(b, sizeof(b), "%d min %d s", (int)(s / 60), (int)(s % 60));
    else snprintf(b, sizeof(b), "%d h %d min", (int)(s / 3600), (int)(s / 60 % 60));
    return b;
}

static void drop_client(client_t *c, const char *why) {
    if (c->fd < 0) return;
    closesocket(c->fd);
    c->fd = -1;
    c->line_len = 0;
    printf("rfc2217: client %s %s\n", c->ip, why);
    char b[160];
    snprintf(b, sizeof(b), "client :%u %s after %s: %lu bytes in, %lu out, %lu commands sent, %lu not sent (%d of %d connected)",
             (unsigned)c->port, why, span(now_ms() - c->since_ms).c_str(), (unsigned long)c->rx,
             (unsigned long)c->tx, (unsigned long)c->commands, (unsigned long)c->refused, connected_count(),
             MAX_CLIENTS);
    note(c, b);
    c->ip[0] = 0;
}

// A line the client typed, in the traffic log (it has already gone to the SVS)
static void log_line(client_t *c) {
    size_t start = 0, end = c->line_len;
    while (start < end && (c->line[start] == '\r' || c->line[start] == '\n')) start++;
    while (end > start && (c->line[end - 1] == '\r' || c->line[end - 1] == '\n')) end--;
    c->line_len = 0;
    if (end == start) return;
    c->commands++;
    const std::string cmd((const char *)c->line + start, end - start);
    note(c, "to the SVS: " + cmd);
    // Y<n> / G<n>: the SVS answers with the input number and then the value; see rfc2217_echo_t
    if (cmd.size() > 1 && (cmd[0] == 'Y' || cmd[0] == 'G') && cmd.size() < 5 &&
        cmd.find_first_not_of("0123456789", 1) == std::string::npos) {
        rfc2217_echo_expect(&c->echo, atoi(cmd.c_str() + 1));
        c->echo_ms = now_ms();
    }
}

// Bytes from the client go to the SVS as they come, like a serial cable: the SVS's own tools send
// blank lines and pairs of terminators and count the answers, so nothing is regrouped or trimmed.
static void from_client(client_t *c, const uint8_t *data, size_t len) {
    const esp_err_t err = svs_settings::busy() ? ESP_ERR_NOT_ALLOWED : svs_usb::send_raw(data, len);
    if (err != ESP_OK) {
        c->refused++;
        note(c, std::string("not sent to the SVS (") +
                    (err == ESP_ERR_INVALID_STATE ? "SVS not connected"
                     : err == ESP_ERR_NOT_ALLOWED ? "SVS busy: settings or firmware update in progress"
                                                  : "send error") +
                    "): " + describe(data, len));
        c->line_len = 0;
        return;
    }
    for (size_t i = 0; i < len; i++) {
        if (c->line_len < sizeof(c->line)) c->line[c->line_len++] = data[i];
        if (data[i] == '\n' || c->line_len == sizeof(c->line)) log_line(c);
    }
}

// A client raising DTR, as opening a serial port does, restarts the SVS like it would a board with an
// auto-reset line: the official utility waits for the banner that follows. Video drops for a moment.
static void restart_for_dtr(client_t *c) {
    static int64_t last_ms = -RESTART_MIN_GAP_MS;
    const int64_t now = now_ms();
    if (now - last_ms < RESTART_MIN_GAP_MS) {
        note(c, "DTR raised: not restarting the SVS, it was restarted moments ago");
        return;
    }
    if (svs_settings::busy()) {
        note(c, "DTR raised: not restarting the SVS, its settings are being read or written");
        return;
    }
    const esp_err_t err = svs_usb::restart_svs();
    if (err == ESP_OK) last_ms = now;
    note(c, err == ESP_OK                  ? "DTR raised: restarting the SVS"
            : err == ESP_ERR_INVALID_STATE ? "DTR raised: not restarting the SVS, it is not connected"
                                           : "DTR raised: could not restart the SVS (firmware update in progress?)");
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
    c->since_ms = c->last_rx_ms = now_ms();
    c->rx = c->tx = c->commands = c->refused = 0;
    inet_ntoa_r(peer->sin_addr, c->ip, sizeof(c->ip));
    c->port = ntohs(peer->sin_port);
    rfc2217_init(&c->proto);
    c->rx_pos = svs_usb::rx_head();  // from now on, not what came before
    rfc2217_echo_init(&c->echo);
    c->modem_sent = -1;
    c->line_len = 0;
    const size_t n = rfc2217_greeting(&c->proto, buf, size);
    if (!send_to(c, buf, n)) {
        drop_client(c, "failed");
    } else {
        printf("rfc2217: client %s connected\n", c->ip);
        char b[160];
        snprintf(b, sizeof(b), "client connected from port %u (%d of %d connected); offered: ", (unsigned)c->port,
                 connected_count(), MAX_CLIENTS);
        note(c, b + describe(buf, n));
    }
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
                c->rx += (uint32_t)n;
                c->last_rx_ms = now_ms();
                rfc2217_io_t io = {};
                io.data = data;
                io.data_max = sizeof(data);
                io.reply = reply;
                io.reply_max = sizeof(reply);
                rfc2217_input(&c->proto, in, (size_t)n, &io, modem);
                // What the client sent (Telnet/RFC 2217 negotiation and any text) and the answer
                note(c, "received " + std::to_string(n) + " bytes: " + describe(in, (size_t)n) +
                            (io.reply_len ? "; answered: " + describe(reply, io.reply_len) : ""));
                if (io.reply_len && !send_to(c, reply, io.reply_len)) {
                    drop_client(c, "failed");
                    continue;
                }
                if (io.dtr_raised) restart_for_dtr(c);
                if (io.data_len) from_client(c, data, io.data_len);
                if (c->fd < 0) continue;
            }
            if (c->fd < 0) continue;

            // The modem state, announced at connect and on every change.
            if (modem != c->modem_sent) {
                const size_t n = rfc2217_modemstate(modem, out, sizeof(out));
                if (!send_to(c, out, n)) {
                    drop_client(c, "failed");
                    continue;
                }
                c->modem_sent = modem;
            }

            // What the SVS says, byte for byte (but for the input number before a Y/G value).
            uint8_t raw[128], out[sizeof(raw) + RFC2217_ECHO_HOLD_MAX];
            if (rfc2217_echo_pending(&c->echo) && now_ms() - c->echo_ms > ECHO_WAIT_MS) {
                const size_t m = rfc2217_echo_release(&c->echo, out, sizeof(out));
                if (m && !send_text(c, std::string((const char *)out, m))) {
                    drop_client(c, "failed");
                    continue;
                }
            }
            for (size_t n; (n = svs_usb::rx_since(c->rx_pos, raw, sizeof(raw))) > 0;) {
                const uint32_t dropped = c->echo.dropped;
                const size_t m = rfc2217_echo_filter(&c->echo, raw, n, out, sizeof(out));
                // What the client is given is noted for the first minute of a connection and for a few
                // seconds after it sends something: tools that count lines (the official utility) read
                // the wrong one if it differs from a real cable.
                const int64_t now = now_ms();
                if (now - c->since_ms < TRACE_MS || now - c->last_rx_ms < TRACE_AFTER_TX_MS) {
                    note(c, "to the client: " + (m ? describe(out, m) : std::string("(held)")) +
                                (c->echo.dropped != dropped ? " (without the input number the SVS puts before a Y/G value)" : ""));
                }
                if (m && !send_text(c, std::string((const char *)out, m))) {
                    drop_client(c, "failed");
                    break;
                }
            }
        }
    }
}

int rfc2217_count(int *max) {
    if (max) *max = MAX_CLIENTS;
    return connected_count();
}

int rfc2217_info(rfc2217_info_t *out, int max) {
    const int64_t now = now_ms();
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS && n < max; i++) {
        const client_t *c = &clients[i];
        if (c->fd < 0) continue;
        rfc2217_info_t *o = &out[n++];
        snprintf(o->ip, sizeof(o->ip), "%s", c->ip);
        o->port = c->port;
        o->connected_s = (uint32_t)((now - c->since_ms) / 1000);
        o->idle_s = (uint32_t)((now - c->last_rx_ms) / 1000);
        o->rx = c->rx;
        o->tx = c->tx;
        o->commands = c->commands;
        o->refused = c->refused;
    }
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
