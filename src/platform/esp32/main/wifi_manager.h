// WiFi connection management with a setup portal fallback.
//
// With stored credentials the device joins that network as a station. If it
// has none, or cannot connect within a few seconds, it also brings up its own
// access point ("SVS-Bridge-XXXX") with a captive portal to configure WiFi.
// The access point shuts down once the station connection is up.
//
// Requires NVS, esp_netif and the default event loop to be initialized.
#pragma once

#include <string>
#include "esp_err.h"

namespace wifi_manager {

void start();

// Stable per-device identifier "svs-bridge-aabbccddeeff" from the WiFi MAC.
// Survives factory resets (it is not stored), so it is safe as a unique_id.
std::string device_id();

bool sta_connected();
bool ap_active();
std::string sta_ssid();
std::string sta_ip();
int sta_rssi();
std::string ap_ssid();

// Scans for networks and returns them as a JSON array:
// [{"ssid":"...","rssi":-50,"secure":true}, ...]
std::string scan_json();

// Stores new credentials and connects to that network.
esp_err_t set_credentials(const std::string &ssid, const std::string &password);

}  // namespace wifi_manager
