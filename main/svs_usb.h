// USB host link to the SVS (Scalable Video Switch) through its CH340.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "esp_err.h"

namespace svs_usb {

// Installs the USB host stack and starts the tasks that open the SVS when it
// is plugged in and print everything it sends. Returns immediately.
void start();

bool is_connected();

// What the SVS reports: its firmware version only in the banner it prints
// when it boots (SVS_FW_1.21), the input count and current input every 2 s
// (SVS TOTAL INPUTS=n, SVS CURRENT INPUT=n; 0 = no input selected) and after
// each input change (SVS NEW INPUT=n). There is no command to ask for it.
//
// The last known values are kept in NVS, so the bridge knows them after a
// restart of its own without restarting the SVS. Only when nothing is known
// (first install, factory reset) is the SVS restarted once when it connects.
struct Info {
    std::string firmware;  // e.g. "SVS_FW_1.21", empty if never seen
    int current_input;     // -1 if unknown
    int total_inputs;      // -1 if unknown
    bool live;             // firmware seen since the bridge started (else: last known)
    bool inputs_live;      // inputs seen since the bridge started (else: last known)
    uint32_t boots_seen;   // banners seen since the bridge started
    uint32_t connections;  // times the SVS was plugged in (changes on replug)
};
Info info();

// Whether the bridge may talk back to the SVS (send commands, reset it, enter
// its bootloader, flash it) or only listen. Listening always works; sending
// only works with the RetroTINK's HD-15 disconnected, so this lets the user
// keep the bridge in a safe listen-only mode during normal use. The choice
// persists in NVS. Default: listen only.
bool send_allowed();
void set_send_allowed(bool allowed);

// Restarts the SVS by pulsing DTR, as the official utility does when it
// connects; it then prints its banner. Video drops for a moment.
// ESP_ERR_NOT_ALLOWED during a firmware update.
esp_err_t restart_svs();

// Sends a command to the SVS, appending the configured line ending.
// ESP_ERR_INVALID_STATE if the SVS is not connected, ESP_ERR_NOT_ALLOWED
// while a firmware update holds the link.
esp_err_t send(const std::string &cmd);

// --- Traffic log -------------------------------------------------------------
//
// The last lines exchanged with the SVS, for the web UI. dir is '<' for text
// received from the SVS, '>' for commands sent to it, '*' for link events.

struct LogEntry {
    uint32_t seq;  // increasing; entries with seq > after are new
    int64_t ms;    // bridge uptime
    char dir;
    std::string text;
};

// Entries with seq > after (at most max), oldest first
std::vector<LogEntry> log_since(uint32_t after, size_t max);

// Adds a link event ('*') to the log, e.g. from the firmware updater
void log_note(const std::string &text);

// --- Exclusive raw access, for flashing the SVS firmware --------------------
//
// While held, send() is refused and received bytes go to raw_read() instead of
// the log. raw_end() restores the normal baud rate and logging.

esp_err_t raw_begin(uint32_t baudrate);
void raw_end();
bool raw_active();

esp_err_t raw_set_lines(bool dtr, bool rts);
esp_err_t raw_set_baudrate(uint32_t baudrate);
esp_err_t raw_write(const uint8_t *data, size_t len);

// Waits until len bytes arrive or timeout_ms passes; returns the bytes read.
size_t raw_read(uint8_t *buf, size_t len, uint32_t timeout_ms);

// Discards input until the line has been quiet for quiet_ms.
void raw_drain(uint32_t quiet_ms);

}  // namespace svs_usb
