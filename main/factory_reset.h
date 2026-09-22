// Factory reset: erases all settings (NVS: WiFi, TLS certificate, ...) and
// reboots, so the bridge comes back in setup mode. The firmware is untouched.
//
// Triggered from the web UI or by holding the BOOT button while the bridge is
// running. (Holding BOOT while powering up enters the ROM download mode
// instead, as on any ESP32.)
#pragma once

namespace factory_reset {

// Starts watching the reset button. Requires NVS to be initialized.
void start_button_monitor();

// Erases NVS and reboots. Does not return.
[[noreturn]] void perform();

}  // namespace factory_reset
