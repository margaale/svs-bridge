// USB host link to the SVS.
//
// The ESP32-S3 acts as USB host (native "USB" port) and opens the CH340 of
// the SVS. Everything the SVS sends is printed on the console.

#include "svs_usb.h"
#include "svs_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "usb/usb_host.h"
#include "usb/usb_helpers.h"
#include "usb/cdc_acm_host.h"
#include "usb/vcp.hpp"
#include "usb/vcp_ch34x.hpp"

using namespace esp_usb;

namespace svs_usb {

using namespace svs_protocol;  // parse_svs_line, only_control_chars, StatusFilter, ...

static const char *TAG = "svs_usb";

#ifdef CONFIG_SVS_ASSERT_DTR
static const bool ASSERT_DTR = true;
#else
static const bool ASSERT_DTR = false;
#endif

#if CONFIG_SVS_EOL_LF
static const char *EOL = "\n";
#elif CONFIG_SVS_EOL_CR
static const char *EOL = "\r";
#elif CONFIG_SVS_EOL_CRLF
static const char *EOL = "\r\n";
#else
static const char *EOL = "";
#endif

struct new_dev_t {
    uint16_t vid;
    uint16_t pid;
};

static QueueHandle_t s_new_dev_q;
static SemaphoreHandle_t s_disconnected;
static SemaphoreHandle_t s_dev_mutex;
static StreamBufferHandle_t s_rx_stream;   // normal mode: to the log
static StreamBufferHandle_t s_raw_stream;  // raw mode: to raw_read()
static CdcAcmDevice *s_dev = nullptr;      // guarded by s_dev_mutex
static volatile bool s_raw = false;
static Info s_info = {"", -1, -1, false, false, 0, 0};  // guarded by s_dev_mutex
static volatile bool s_send_allowed = false;  // listen-only until enabled

// If a just-connected SVS has never reported anything, wait this long for a
// banner (it may be booting too) before restarting it to get one
static const int FIRST_BANNER_WAIT_MS = 4000;

static const char *NVS_NAMESPACE = "svs";

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

// ---------------------------------------------------------------------------
// Traffic log
// ---------------------------------------------------------------------------

static const size_t LOG_CAPACITY = 300;
static SemaphoreHandle_t s_log_mutex = nullptr;
static std::deque<LogEntry> s_log;
static uint32_t s_log_seq = 0;

static void log_add(char dir, const std::string &text)
{
    if (s_log_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    s_log.push_back({++s_log_seq, now_ms(), dir, text});
    if (s_log.size() > LOG_CAPACITY) {
        s_log.pop_front();
    }
    xSemaphoreGive(s_log_mutex);
}

void log_note(const std::string &text)
{
    log_add('*', text);
}

std::vector<LogEntry> log_since(uint32_t after, size_t max)
{
    std::vector<LogEntry> out;
    xSemaphoreTake(s_log_mutex, portMAX_DELAY);
    for (const auto &e : s_log) {
        if (e.seq > after) {
            out.push_back(e);
            if (out.size() >= max) {
                break;
            }
        }
    }
    xSemaphoreGive(s_log_mutex);
    return out;
}

// ---------------------------------------------------------------------------
// USB callbacks
// ---------------------------------------------------------------------------

// Called for every newly connected device, CDC/VCP or not.
static void on_new_device(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *dev_desc;
    if (usb_host_get_device_descriptor(usb_dev, &dev_desc) != ESP_OK) {
        return;
    }
    usb_device_info_t info;
    usb_host_device_info(usb_dev, &info);

    printf("\n=== USB device connected: VID=%04X PID=%04X (%s speed) ===\n",
           dev_desc->idVendor, dev_desc->idProduct,
           info.speed == USB_SPEED_LOW ? "low" : "full");
    if (info.str_desc_manufacturer) {
        usb_print_string_descriptor(info.str_desc_manufacturer);
    }
    if (info.str_desc_product) {
        usb_print_string_descriptor(info.str_desc_product);
    }
    usb_print_device_descriptor(dev_desc);
    const usb_config_desc_t *cfg_desc;
    if (usb_host_get_active_config_descriptor(usb_dev, &cfg_desc) == ESP_OK) {
        usb_print_config_descriptor(cfg_desc, NULL);
    }

    new_dev_t nd = {dev_desc->idVendor, dev_desc->idProduct};
    xQueueSend(s_new_dev_q, &nd, 0);
}

static bool on_rx(const uint8_t *data, size_t data_len, void *arg)
{
    size_t sent = xStreamBufferSend(s_raw ? s_raw_stream : s_rx_stream, data, data_len, 0);
    if (sent < data_len) {
        ESP_LOGW(TAG, "RX buffer full, dropped %u bytes", (unsigned)(data_len - sent));
    }
    return true;
}

static void on_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error: %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "SVS disconnected");
        log_add('*', "SVS disconnected");
        xSemaphoreGive(s_disconnected);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGI(TAG, "Serial state: 0x%04X", event->data.serial_state.val);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------

static void usb_lib_task(void *arg)
{
    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

// only_control_chars, is_repeated_status (as StatusFilter) and trailing_number
// live in svs_protocol.h so they can be unit-tested on the host.
static StatusFilter s_status_filter;

static void print_line(std::string &line)
{
    printf("%8lld ms  SVS > %s\n", now_ms(), line.c_str());
    if (!only_control_chars(line) && !s_status_filter.is_repeated(line)) {
        log_add('<', line);
    }
    line.clear();
}

// SVS info is only ever what a currently connected SVS has reported: it is not
// cached in NVS and is cleared on disconnect (see device_task). If the SVS is
// unplugged we report nothing rather than stale values from a past session.

// Picks the banner and input change lines out of what the SVS sends
static void parse_line(const std::string &line)
{
    ParsedLine p = parse_svs_line(line);
    if (p.kind == LineKind::None) {
        return;
    }
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    switch (p.kind) {
    case LineKind::Firmware:
        s_info.firmware = line;
        s_info.live = true;
        s_info.boots_seen++;
        break;
    case LineKind::InputChange:
        s_info.current_input = p.value;
        s_info.inputs_live = true;
        break;
    case LineKind::TotalInputs:
        s_info.total_inputs = p.value;
        s_info.inputs_live = true;
        break;
    case LineKind::None:
        break;  // handled above
    }
    xSemaphoreGive(s_dev_mutex);
}

// Prints data received from the SVS and parses its banner. In text mode it
// groups by line; if no line ending arrives within 50 ms, whatever is pending
// is printed anyway.
static void rx_print_task(void *arg)
{
    uint8_t buf[128];
    std::string line;    // for display, non-printables as <XX>
    std::string parsed;  // printable characters only, for parse_line()
    while (true) {
        size_t n = xStreamBufferReceive(s_rx_stream, buf, sizeof(buf), pdMS_TO_TICKS(50));
#if CONFIG_SVS_HEX_DUMP
        if (n > 0) {
            std::string hex;
            for (size_t i = 0; i < n; i++) {
                char b[4];
                snprintf(b, sizeof(b), i ? " %02X" : "%02X", buf[i]);
                hex += b;
            }
            printf("%8lld ms  SVS > %s\n", now_ms(), hex.c_str());
            log_add('<', hex);
        }
#else
        if (n == 0 && !line.empty()) {
            print_line(line);
        }
#endif
        for (size_t i = 0; i < n; i++) {
            uint8_t c = buf[i];
            if (c == '\n' || c == '\r') {
                if (!parsed.empty()) {
                    parse_line(parsed);
                    parsed.clear();
                }
#if !CONFIG_SVS_HEX_DUMP
                if (!line.empty()) {
                    print_line(line);
                }
#endif
                continue;
            }
            if (c >= 0x20 && c < 0x7F) {
                if (parsed.size() < 128) {
                    parsed += (char)c;
                }
            }
#if !CONFIG_SVS_HEX_DUMP
            if (c >= 0x20 && c < 0x7F) {
                line += (char)c;
            } else {
                char hex[8];
                snprintf(hex, sizeof(hex), "<%02X>", c);
                line += hex;
            }
            if (line.size() >= 256) {
                print_line(line);
            }
#endif
        }
    }
}

static esp_err_t set_baudrate(CdcAcmDevice *dev, uint32_t baudrate)
{
    cdc_acm_line_coding_t line_coding = {};
    line_coding.dwDTERate = baudrate;
    line_coding.bCharFormat = 0;  // 1 stop bit
    line_coding.bParityType = 0;  // no parity
    line_coding.bDataBits = 8;
    return dev->line_coding_set(&line_coding);
}

// Opens the SVS whenever a device is plugged in and waits for it to go away.
static void device_task(void *arg)
{
    while (true) {
        new_dev_t nd;
        xQueueReceive(s_new_dev_q, &nd, portMAX_DELAY);

        cdc_acm_host_device_config_t dev_config = {};
        dev_config.connection_timeout_ms = 3000;
        dev_config.out_buffer_size = 512;
        dev_config.in_buffer_size = 512;
        dev_config.event_cb = on_event;
        dev_config.data_cb = on_rx;
        dev_config.user_arg = NULL;

        CdcAcmDevice *dev = VCP::open(&dev_config);
        char event[80];
        if (dev == nullptr) {
            ESP_LOGW(TAG, "VID=%04X PID=%04X is not a supported CH34x", nd.vid, nd.pid);
            snprintf(event, sizeof(event), "USB device %04X:%04X is not an SVS (CH34x)", nd.vid, nd.pid);
            log_add('*', event);
            continue;
        }

        ESP_ERROR_CHECK_WITHOUT_ABORT(set_baudrate(dev, CONFIG_SVS_BAUDRATE));
        ESP_ERROR_CHECK_WITHOUT_ABORT(dev->set_control_line_state(ASSERT_DTR, false));

        ESP_LOGI(TAG, "SVS opened at %d 8N1", CONFIG_SVS_BAUDRATE);
        snprintf(event, sizeof(event), "SVS connected (USB %04X:%04X, %d 8N1)", nd.vid, nd.pid,
                 CONFIG_SVS_BAUDRATE);
        log_add('*', event);

        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        s_dev = dev;
        s_info.connections++;
        bool known = !s_info.firmware.empty();
        uint32_t boots = s_info.boots_seen;
        xSemaphoreGive(s_dev_mutex);

        // Nothing known about this SVS yet (first install, factory reset):
        // unless it is booting right now and prints its banner by itself,
        // restart it once to learn its firmware version and inputs. Only in
        // send mode: listen-only must never reset the SVS (it drops video and
        // would fail with the RetroTINK's HD-15 connected anyway).
        if (!known && s_send_allowed) {
            for (int i = 0; i < FIRST_BANNER_WAIT_MS / 100 && info().boots_seen == boots; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (info().boots_seen == boots && is_connected()) {
                log_add('*', "First time with this SVS: restarting it once to read its version");
                restart_svs();
            }
        }

        xSemaphoreTake(s_disconnected, portMAX_DELAY);

        // The SVS is gone: drop everything we knew about it (keep only the
        // lifetime connection count) so we never report stale values.
        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        s_dev = nullptr;
        s_info.firmware.clear();
        s_info.current_input = -1;
        s_info.total_inputs = -1;
        s_info.live = false;
        s_info.inputs_live = false;
        s_info.boots_seen = 0;
        xSemaphoreGive(s_dev_mutex);
        delete dev;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void start()
{
    s_new_dev_q = xQueueCreate(4, sizeof(new_dev_t));
    s_disconnected = xSemaphoreCreateBinary();
    s_dev_mutex = xSemaphoreCreateMutex();
    s_rx_stream = xStreamBufferCreate(4096, 1);
    s_raw_stream = xStreamBufferCreate(1024, 1);
    s_log_mutex = xSemaphoreCreateMutex();

    nvs_handle_t h;
    uint8_t send = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "send", &send);
        nvs_close(h);
    }
    s_send_allowed = send != 0;
    ESP_LOGI(TAG, "SVS mode: %s", s_send_allowed ? "send allowed" : "listen only");

    usb_host_config_t host_config = {};
    host_config.skip_phy_setup = false;
    host_config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);

    cdc_acm_host_driver_config_t driver_config = {};
    driver_config.driver_task_stack_size = 4096;
    driver_config.driver_task_priority = 10;
    driver_config.xCoreID = 0;
    driver_config.new_dev_cb = on_new_device;
    ESP_ERROR_CHECK(cdc_acm_host_install(&driver_config));

    VCP::register_driver<CH34x>();

    xTaskCreate(rx_print_task, "svs_rx", 4096, NULL, 5, NULL);
    xTaskCreate(device_task, "svs_dev", 4096, NULL, 5, NULL);
}

bool is_connected()
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    bool connected = s_dev != nullptr;
    xSemaphoreGive(s_dev_mutex);
    return connected;
}

Info info()
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    Info copy = s_info;
    xSemaphoreGive(s_dev_mutex);
    return copy;
}

bool send_allowed() { return s_send_allowed; }

void set_send_allowed(bool allowed)
{
    s_send_allowed = allowed;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "send", allowed ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "SVS mode: %s", allowed ? "send allowed" : "listen only");
}

esp_err_t restart_svs()
{
    if (!s_send_allowed) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    if (s_dev == nullptr || s_raw) {
        xSemaphoreGive(s_dev_mutex);
        return s_raw ? ESP_ERR_NOT_ALLOWED : ESP_ERR_INVALID_STATE;
    }
    // Asserting DTR pulls the CH340's DTR# low, which the SVS turns into a
    // reset pulse (Arduino-style auto reset)
    esp_err_t err = s_dev->set_control_line_state(true, true);
    xSemaphoreGive(s_dev_mutex);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    if (s_dev != nullptr) {
        err = s_dev->set_control_line_state(ASSERT_DTR, false);
    }
    xSemaphoreGive(s_dev_mutex);
    log_add('*', "SVS restarted");
    return err;
}

esp_err_t send(const std::string &cmd)
{
    if (!s_send_allowed) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    std::string out = cmd + EOL;
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    if (s_dev == nullptr || s_raw) {
        xSemaphoreGive(s_dev_mutex);
        return s_raw ? ESP_ERR_NOT_ALLOWED : ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = s_dev->tx_blocking((uint8_t *)out.data(), out.size(), 1000);
    xSemaphoreGive(s_dev_mutex);
    if (err == ESP_OK) {
        printf("%8lld ms  SVS < %s\n", now_ms(), cmd.c_str());
        log_add('>', cmd);
    } else {
        log_add('*', "Could not send " + cmd + ": " + esp_err_to_name(err));
    }
    return err;
}

// ---------------------------------------------------------------------------
// Raw access
// ---------------------------------------------------------------------------

esp_err_t raw_begin(uint32_t baudrate)
{
    if (!s_send_allowed) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    if (s_dev == nullptr || s_raw) {
        xSemaphoreGive(s_dev_mutex);
        return s_raw ? ESP_ERR_NOT_ALLOWED : ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = set_baudrate(s_dev, baudrate);
    if (err == ESP_OK) {
        xStreamBufferReset(s_raw_stream);
        s_raw = true;
        ESP_LOGI(TAG, "Raw mode at %u baud", (unsigned)baudrate);
        log_add('*', "Bootloader session started (restarting the SVS)");
    }
    xSemaphoreGive(s_dev_mutex);
    return err;
}

void raw_end()
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    if (s_raw) {
        s_raw = false;
        if (s_dev != nullptr) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(set_baudrate(s_dev, CONFIG_SVS_BAUDRATE));
            ESP_ERROR_CHECK_WITHOUT_ABORT(s_dev->set_control_line_state(ASSERT_DTR, false));
        }
        ESP_LOGI(TAG, "Raw mode ended");
        log_add('*', "Bootloader session ended");
    }
    xSemaphoreGive(s_dev_mutex);
}

bool raw_active() { return s_raw; }

esp_err_t raw_set_lines(bool dtr, bool rts)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    esp_err_t err = (s_dev != nullptr && s_raw) ? s_dev->set_control_line_state(dtr, rts)
                                                : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_dev_mutex);
    return err;
}

esp_err_t raw_set_baudrate(uint32_t baudrate)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    esp_err_t err = (s_dev != nullptr && s_raw) ? set_baudrate(s_dev, baudrate) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_dev_mutex);
    return err;
}

esp_err_t raw_write(const uint8_t *data, size_t len)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    esp_err_t err = (s_dev != nullptr && s_raw) ? s_dev->tx_blocking((uint8_t *)data, len, 1000)
                                                : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_dev_mutex);
    return err;
}

size_t raw_read(uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    size_t got = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (got < len) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            break;
        }
        got += xStreamBufferReceive(s_raw_stream, buf + got, len - got, deadline - now);
    }
    return got;
}

void raw_drain(uint32_t quiet_ms)
{
    uint8_t discard[64];
    while (xStreamBufferReceive(s_raw_stream, discard, sizeof(discard), pdMS_TO_TICKS(quiet_ms)) > 0) {
    }
}

}  // namespace svs_usb
