// The SVS's own settings, and the bridge's description of the switch.
//
// Settings stored in the SVS's EEPROM (per-input auto profile, sync on green,
// sync bypass, transcoders and IR codes), read and written as the official
// SVS Management Utility does (see svs_config.h). Both need the bridge to send
// to the SVS, so the RetroTINK's HD-15 must be unplugged; the web UI asks for
// that before each read or write. Nothing is written that was not read first,
// and only the bytes that change.
//
// The layout is what the SVS cannot report: which kind of module each input
// is (only V3 modules identify themselves; "" until it is picked), the
// outputs, and names. The outputs run from the control module outwards and
// hold the transcoders too ("tx_rgb_to_ypbpr", "tx_ypbpr_to_rgb"), where they
// sit: a transcoder converts only the outputs past it. It is kept in the
// bridge's NVS as JSON:
//   {"inputs": [{"kind": "scart", "name": "Super Nintendo"}, ...],
//    "outputs": [{"kind": "tx_rgb_to_ypbpr", "name": ""},
//                {"kind": "component", "name": "RetroTINK 4K"}, ...]}
#pragma once

#include <stdint.h>
#include <string>
#include <vector>
#include "esp_err.h"
#include "svs_config.h"

namespace svs_settings {

void init();

// --- Settings stored in the SVS -------------------------------------------------

enum class Task { Idle, Reading, Writing };

struct Snapshot {
    bool valid;               // read from the SVS that is plugged in now
    uint32_t seq;             // changes with every read or write
    int64_t age_ms;           // since it was read
    std::string firmware;
    int inputs;
    int ir_slots;             // IR codes each input can hold
    svs_config::Hardware hardware;
    std::vector<svs_config::InputSettings> settings;  // index 0 = input 1
};

struct Status {
    Task task;
    std::string phase;
    int progress;             // 0..100 while reading or writing
    std::string result;       // how the last read or write ended
    bool result_ok;
    Task result_of;           // Idle if there is no result yet
    std::string blocker;      // why a read cannot start now ("" if it can)
    Snapshot snapshot;
};

Status status();
bool busy();

// In the background. ESP_ERR_NOT_SUPPORTED in listen-only mode,
// ESP_ERR_INVALID_STATE with *error set if it cannot start.
esp_err_t start_read(std::string *error);

// Writes `want` (one entry per input the SVS reported) over the last read.
// Settings a module cannot have (sync on green without a V3 module, a
// transcoder that is not fitted) keep what was read.
esp_err_t start_write(const std::vector<svs_config::InputSettings> &want, std::string *error);

// --- The layout (kept on the bridge) ---------------------------------------------

std::string layout_json();  // "{}" if none saved

// Validates and stores it. ESP_ERR_INVALID_ARG with *error on a bad layout.
esp_err_t set_layout_json(const std::string &json, std::string *error);

// The name given to an input (1-based), "" if none
std::string input_name(int input);

}  // namespace svs_settings
