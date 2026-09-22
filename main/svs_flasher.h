// SVS firmware updates over USB.
//
// The SVS runs an ATmega328P with an urboot bootloader, flashed by the
// official tool as `avrdude -c urclock -p m328p -b 115200 -D`. This module
// speaks the same urclock protocol (https://github.com/stefanrueger/urboot/
// blob/main/urprotocol.md) from the ESP32.
//
// Like the official utility and esp-link, the device is identified in a step
// of its own before any firmware is chosen:
//
//  1. Check: reset the SVS into its bootloader, identify it and leave without
//     writing. It must be urboot on an ATmega328P that can read flash. The SVS
//     then restarts and reports its firmware version.
//  2. Stage: the .hex is fully validated (record checksums and types, fits
//     the ATmega328P) and must fit below the bootloader found in step 1.
//  3. Flash: identify again and abort unless it is the same bootloader as in
//     step 1; write; read everything back and compare; wait for the SVS to
//     report the new version.
//
// The SVS's bootloader is a "vector bootloader": the application reaches it
// through the reset vector, so a firmware image must be patched to keep the
// reset vector pointing at the bootloader (avrdude does the same). Getting
// this wrong bricks the device, so a vector bootloader is only flashed after
// a preview confirms the bridge's patch matches what is already on the chip.
//
// If writing is interrupted, the reset page is written first and patched, so
// the reset vector still points at the bootloader: flash again.
#pragma once

#include <stddef.h>
#include <string>
#include "esp_err.h"

namespace svs_flasher {

enum class Task { Idle, Checking, Flashing, Probing, Previewing };

struct Device {
    bool checked;          // identified since the SVS was last plugged in
    bool compatible;       // an urboot the bridge can flash
    bool vector;           // vector bootloader: the reset vector must be patched
    bool vector_verified;  // a preview confirmed the patch against the chip
    std::string summary;   // e.g. "ATmega328P · urboot v8.0 · 256-byte bootloader · vector boot"
    std::string problem;   // why it cannot be flashed (!compatible), or the vector-boot note
    size_t app_space;      // flash bytes below the bootloader (also the bootloader start)
    int vector_num;        // application vector number for the patch, when vector
};

struct Image {
    bool staged;
    std::string source;   // e.g. "SVS_FW_1.21.hex (official repository)"
    size_t size;          // bytes of flash the image occupies
    std::string sha256;   // of the decoded flash image
};

struct Status {
    Task task;
    std::string phase;    // what the running task is doing
    int progress;         // 0..100 while flashing
    Device device;
    Image image;
    bool can_flash;       // everything is in place to flash
    std::string blocker;  // why not, when !can_flash
    std::string result;   // outcome of the last check or flash
    bool result_ok;
    Task result_of;       // Checking or Flashing; Idle if there is no result yet
};

void init();

// Step 1, in the background. ESP_ERR_INVALID_STATE if busy or no SVS.
esp_err_t start_check();

// Step 2. On failure *error says why and nothing stays staged, so only what
// was chosen last can ever be flashed.
esp_err_t stage_hex(const char *hex, size_t len, const std::string &source, std::string *error);

// Drops the staged image. No-op while busy.
void clear();

// For a vector bootloader: reads the reset vector from the chip and checks
// that the patch the bridge would write matches what the official tool already
// put there. Writes nothing. On success device.vector_verified becomes true,
// which flashing a vector bootloader requires. In the background.
esp_err_t start_preview();

// Step 3, in the background. ESP_ERR_INVALID_STATE unless status().can_flash.
esp_err_t start_flash();

// Diagnostics for when the bootloader does not answer, in the background:
// restarts the SVS several times, listening and trying to sync with different
// timings and baud rates, and writes what it finds to the SVS log (see
// svs_usb::log_since). Never writes to the SVS.
esp_err_t start_probe();

bool busy();
Status status();

}  // namespace svs_flasher
