// Web servers: UI, admin login, WiFi setup and firmware upload (OTA).
//
// /api/ is reserved for external integrations (e.g. Home Assistant, and later
// the SVS control API). /device/ holds the endpoints the web UI uses to manage
// the bridge itself; they are not part of the public API.
//
// HTTPS (443). [admin] = needs a logged-in session (cookie); [api] = needs a
// valid "Authorization: Bearer <API token>" header:
//   GET  /                        web UI
//   GET  /api/status              firmware, WiFi, TLS and SVS status (JSON)
//   GET  /api/v1/info             [api] device identity and capabilities (JSON)
//   GET  /api/v1/state            [api] live SVS and bridge state (JSON)
//   GET  /device/api-token        [admin] the current API token (JSON)
//   POST /device/api-token/regenerate  [admin] mint a new API token (JSON)
//   GET  /device/auth             {"password_set": bool, "authenticated": bool}
//   POST /device/setup-password   {"password"}, only while none is set
//   POST /device/login            {"password"}
//   POST /device/logout
//   GET  /device/scan             [admin] nearby WiFi networks (JSON)
//   GET  /device/cert             [admin] server certificate (PEM)
//   POST /device/wifi             [admin] {"ssid", "password"}
//   POST /device/ota              [admin] raw firmware image (build/svs_bridge.bin)
//   GET  /device/releases         [admin] the bridge's own GitHub releases (JSON)
//   POST /device/ota/github       [admin] {"tag"}: download that release and update
//   POST /device/reboot           [admin]
//   POST /device/factory-reset    [admin] erase all settings, reboot into setup mode
//   GET  /device/svs              [admin] SVS banner info and firmware update state
//   GET  /device/svs/log?after=N  [admin] traffic with the SVS after entry N
//   POST /device/svs/mode         [admin] {"send": bool}: listen-only vs. send to the SVS
//   POST /device/svs/restart      [admin] restart the SVS (it then reports its info)
//   POST /device/svs/send         [admin] {"command"}: send a line to the SVS (web UI console)
//   POST /device/svs/check        [admin] step 1: identify the SVS bootloader (restarts it)
//   POST /device/svs/preview      [admin] validate the vector patch against the chip (writes nothing)
//   POST /device/svs/probe        [admin] bootloader diagnostics into the SVS log (writes nothing)
//   GET  /device/svs/releases     [admin] official SVS firmware releases (GitHub)
//   POST /device/svs/firmware     [admin] step 2: stage an uploaded .hex (body: the file)
//   POST /device/svs/firmware/official  [admin] step 2: {"name"}, download and stage a release
//   POST /device/svs/firmware/flash     [admin] step 3: flash the staged firmware
//
// While the SVS is being flashed, OTA, reboot and factory reset are refused.
//
// HTTP (80), only while the setup access point is up: the setup portal page
// (step 1: create or enter the admin password, step 2: WiFi) and the
// endpoints it needs (/api/status, /device/auth, /device/setup-password,
// /device/login, and [admin] /device/scan and /device/wifi). Every other URL
// redirects to the portal so phones show it as a captive portal; captive
// portal detection only works over plain HTTP. When the access point is down,
// every HTTP request is redirected to HTTPS.
#pragma once

namespace web_server {

// Requires auth::load() and tls_cert::load() to have succeeded.
void start();

}  // namespace web_server
