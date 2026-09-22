#include "dns_server.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

namespace dns_server {

static const char *TAG = "dns";

static const int DNS_PORT = 53;
static const size_t DNS_HEADER_LEN = 12;
static const size_t DNS_MAX_LEN = 512;
static const uint16_t QTYPE_A = 1;
static const uint16_t QCLASS_IN = 1;

static TaskHandle_t s_task = nullptr;
static int s_sock = -1;
static uint32_t s_ip = 0;
static volatile bool s_running = false;

static uint16_t read_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void write_u16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}

// Builds the reply in place. Returns the reply length, or 0 to drop the packet.
static size_t build_reply(uint8_t *buf, size_t len)
{
    if (len < DNS_HEADER_LEN) {
        return 0;
    }
    uint16_t flags = read_u16(buf + 2);
    uint16_t qdcount = read_u16(buf + 4);
    if ((flags & 0x8000) || qdcount != 1) {  // not a query, or not exactly one question
        return 0;
    }

    // Walk the question name (sequence of length-prefixed labels)
    size_t pos = DNS_HEADER_LEN;
    while (pos < len && buf[pos] != 0) {
        if (buf[pos] & 0xC0) {  // compression pointers are not valid in a question
            return 0;
        }
        pos += buf[pos] + 1;
    }
    pos += 1;  // terminating zero
    if (pos + 4 > len) {
        return 0;
    }
    uint16_t qtype = read_u16(buf + pos);
    uint16_t qclass = read_u16(buf + pos + 2);
    size_t question_end = pos + 4;

    bool answer = (qtype == QTYPE_A && qclass == QCLASS_IN);
    const size_t answer_len = 16;
    if (answer && question_end + answer_len > DNS_MAX_LEN) {
        return 0;
    }

    write_u16(buf + 2, 0x8180);          // response, recursion desired + available, no error
    write_u16(buf + 6, answer ? 1 : 0);  // ANCOUNT
    write_u16(buf + 8, 0);               // NSCOUNT
    write_u16(buf + 10, 0);              // ARCOUNT
    if (!answer) {
        return question_end;
    }

    uint8_t *a = buf + question_end;
    write_u16(a + 0, 0xC00C);  // name: pointer to the question name
    write_u16(a + 2, QTYPE_A);
    write_u16(a + 4, QCLASS_IN);
    write_u16(a + 6, 0);       // TTL (high)
    write_u16(a + 8, 60);      // TTL (low): 60 s
    write_u16(a + 10, 4);      // RDLENGTH
    memcpy(a + 12, &s_ip, 4);  // already in network byte order
    return question_end + answer_len;
}

static void dns_task(void *arg)
{
    uint8_t buf[DNS_MAX_LEN];
    while (s_running) {
        struct sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0) {
            continue;  // timeout, or socket closed by stop()
        }
        size_t reply_len = build_reply(buf, (size_t)len);
        if (reply_len > 0) {
            sendto(s_sock, buf, reply_len, 0, (struct sockaddr *)&from, from_len);
        }
    }
    close(s_sock);
    s_sock = -1;
    s_task = nullptr;
    vTaskDelete(NULL);
}

void start(uint32_t ip)
{
    // A previous stop() may still be winding down; give it time to exit
    for (int i = 0; i < 30 && s_task != nullptr && !s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (s_task != nullptr) {
        s_running = true;  // still running (or stop() never finished): keep it
        return;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        return;
    }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed");
        close(s_sock);
        s_sock = -1;
        return;
    }
    // Wake up periodically so stop() can end the task
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    s_ip = ip;
    s_running = true;
    xTaskCreate(dns_task, "dns", 4096, NULL, 5, &s_task);
    ESP_LOGI(TAG, "Captive DNS started");
}

void stop()
{
    if (s_task != nullptr) {
        s_running = false;  // the task exits within 1 s and closes the socket
        ESP_LOGI(TAG, "Captive DNS stopped");
    }
}

}  // namespace dns_server
