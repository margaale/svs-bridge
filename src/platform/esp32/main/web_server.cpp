#include "web_server.h"

#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <string>
#include <memory>
#include <new>
#include <functional>
#include <vector>

#include "sdkconfig.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "ArduinoJson.h"

#include "api_events.h"
#include "auth.h"
#include "bridge_fw_repo.h"
#include "cruller.h"
#include "rfc2217.h"
#include "factory_reset.h"
#include "svs_flasher.h"
#include "svs_fw_repo.h"
#include "svs_settings.h"
#include "svs_usb.h"
#include "tls_cert.h"
#include "wifi_manager.h"

// sdkconfig.defaults sets these, but a build directory configured before keeps its own sdkconfig
#if !CONFIG_HTTPD_WS_SUPPORT || !CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT || CONFIG_LWIP_MAX_SOCKETS < 24
#error "This sdkconfig predates the web UI's WebSocket: delete build/esp32/sdkconfig and build again"
#endif

namespace web_server {

static const char *TAG = "web";

static const size_t MAX_JSON_BODY = 512;
static const size_t MAX_SVS_HEX = 256 * 1024;  // official SVS .hex files are ~85 KB
static const size_t MAX_SETTINGS_BODY = 16384;  // 32 inputs with their IR codes
static const size_t MAX_LAYOUT_BODY = 4096;
static const size_t OTA_CHUNK = 16384;  // one full TLS record per read
static const char *PORTAL_URL = "http://192.168.4.1/";

// The pages, from src/web (embedded by src/web/embed.cmake)
extern "C" const unsigned char web_index_html[];
extern "C" const size_t web_index_html_len;
extern "C" const unsigned char web_portal_html[];
extern "C" const size_t web_portal_html_len;

static const char *SESSION_COOKIE = "svs_session";

typedef esp_err_t (*handler_fn)(httpd_req_t *req);

enum class Access { Public, Admin, Api };

// Registered as user_ctx of every URI; dispatch() applies its rules
struct Route {
    handler_fn handler;
    Access access;
    bool portal_only;  // plain HTTP: only while the setup access point is up
};

static httpd_handle_t s_https_server = nullptr;
static esp_timer_handle_t s_reboot_timer = nullptr;
static volatile bool s_erase_before_reboot = false;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, const JsonDocument &doc)
{
    std::string out;
    serializeJson(doc, out);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out.data(), out.size());
}

static esp_err_t send_ok(httpd_req_t *req)
{
    JsonDocument doc;
    doc["ok"] = true;
    return send_json(req, doc);
}

static esp_err_t send_error(httpd_req_t *req, httpd_err_code_t code, const char *msg)
{
    ESP_LOGW(TAG, "%s: %s", req->uri, msg);
    return httpd_resp_send_err(req, code, msg);
}

static const char *busy_message()
{
    return svs_settings::busy() ? "The SVS's settings are being read or saved"
                                : "An SVS firmware update is in progress";
}

// Rebooting or updating the bridge now would interrupt the SVS update, or
// talking to the SVS now would interleave with reading or saving its settings
static esp_err_t send_busy(httpd_req_t *req)
{
    JsonDocument doc;
    doc["error"] = busy_message();
    httpd_resp_set_status(req, "409 Conflict");
    return send_json(req, doc);
}

// Reads the whole request body. Returns false (and replies if possible) on error.
static bool read_body(httpd_req_t *req, size_t max_len, std::string &body)
{
    if (req->content_len == 0 || req->content_len > max_len) {
        send_error(req, HTTPD_400_BAD_REQUEST, "Missing or too large body");
        return false;
    }
    body.assign(req->content_len, '\0');
    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, &body[received], req->content_len - received);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            return false;  // connection lost, nothing to reply to
        }
        received += r;
    }
    return true;
}

// Reads a small JSON request body. Returns false (and replies) on error.
static bool read_json_body(httpd_req_t *req, JsonDocument &doc)
{
    std::string body;
    if (!read_body(req, MAX_JSON_BODY, body)) {
        return false;
    }
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        send_error(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return false;
    }
    return true;
}

static esp_err_t redirect(httpd_req_t *req, const char *status, const std::string &location)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_hdr(req, "Location", location.c_str());
    return httpd_resp_send(req, NULL, 0);
}

// Same host and path, over HTTPS. 307 keeps the method, so POSTs survive.
static esp_err_t redirect_to_https(httpd_req_t *req)
{
    std::string host = CONFIG_SVS_HOSTNAME ".local";
    size_t len = httpd_req_get_hdr_value_len(req, "Host");
    if (len > 0 && len < 128) {
        char buf[128];
        if (httpd_req_get_hdr_value_str(req, "Host", buf, sizeof(buf)) == ESP_OK) {
            host = buf;
            size_t colon = host.find(':');
            if (colon != std::string::npos) {
                host.resize(colon);  // drop ":80"
            }
        }
    }
    return redirect(req, "307 Temporary Redirect", "https://" + host + req->uri);
}

static void on_reboot_timer(void *arg)
{
    if (s_erase_before_reboot) {
        factory_reset::perform();
    }
    ESP_LOGI(TAG, "Rebooting");
    esp_restart();
}

static void schedule_reboot()
{
    // Give the HTTP response time to reach the browser first
    esp_timer_start_once(s_reboot_timer, 1000 * 1000);
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static esp_err_t send_html(httpd_req_t *req, const unsigned char *page, size_t len)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, reinterpret_cast<const char *>(page), len);
}

static esp_err_t index_get(httpd_req_t *req)
{
    return send_html(req, web_index_html, web_index_html_len);
}

static esp_err_t portal_get(httpd_req_t *req)
{
    return send_html(req, web_portal_html, web_portal_html_len);
}

// What the SVS reported in its last boot banner (null when unknown)
static void add_svs_info(JsonObject svs)
{
    svs_usb::Info info = svs_usb::info();
    svs["connected"] = svs_usb::is_connected();
    if (info.firmware.empty()) {
        svs["firmware"] = nullptr;
    } else {
        svs["firmware"] = info.firmware;
    }
    if (info.current_input >= 0) {
        svs["current_input"] = info.current_input;
    } else {
        svs["current_input"] = nullptr;
    }
    if (info.total_inputs >= 0) {
        svs["total_inputs"] = info.total_inputs;
    } else {
        svs["total_inputs"] = nullptr;
    }
    // false: last known values, from before the bridge restarted
    svs["live"] = info.live;
    svs["inputs_live"] = info.inputs_live;
    svs["send_enabled"] = true;  // there is no listen-only mode any more; kept for integrations
    // The name given to the active input in the web UI ("" if none)
    svs["current_input_name"] = info.current_input > 0 ? svs_settings::input_name(info.current_input) : "";
    // The console picked for it in the web UI (an id such as "snes"; "" if none)
    svs["current_input_device"] = info.current_input > 0 ? svs_settings::input_device(info.current_input) : "";
}

// Why the bridge last started, so unexpected restarts show up remotely
static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "restart";  // reboot button, OTA, factory reset, rollback
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout (supply voltage dropped)";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default: return "unknown";
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    JsonDocument doc;
    JsonObject fw = doc["firmware"].to<JsonObject>();
    fw["project"] = app->project_name;
    fw["version"] = app->version;
    fw["built"] = std::string(app->date) + " " + app->time;
    fw["idf"] = app->idf_ver;
    fw["partition"] = running->label;
    fw["uptime_s"] = esp_timer_get_time() / 1000000;
    fw["reset_reason"] = reset_reason_name(esp_reset_reason());

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["connected"] = wifi_manager::sta_connected();
    wifi["ssid"] = wifi_manager::sta_ssid();
    wifi["ip"] = wifi_manager::sta_ip();
    wifi["rssi"] = wifi_manager::sta_rssi();
    wifi["hostname"] = CONFIG_SVS_HOSTNAME ".local";
    wifi["ap_active"] = wifi_manager::ap_active();
    wifi["ap_ssid"] = wifi_manager::ap_ssid();

    JsonObject tls = doc["tls"].to<JsonObject>();
    tls["source"] = tls_cert::source() == tls_cert::Source::Custom ? "custom" : "self-signed";
    tls["fingerprint"] = tls_cert::fingerprint();

    add_svs_info(doc["svs"].to<JsonObject>());

    return send_json(req, doc);
}

// RFC 2217 clients (the SVS's serial console over the network): who is connected and what each has
// done. Admin only, unlike /api/status: it names addresses on the network.
// {"port", "max", "clients": [{"ip", "port", "connected_s", "idle_s", "rx", "tx", "commands", "refused"}]}
static esp_err_t clients_get(httpd_req_t *req)
{
    JsonDocument doc;
    int max_clients = 0;
    rfc2217_count(&max_clients);
    JsonObject rfc = doc.to<JsonObject>();
    rfc["port"] = 2217;
    rfc["max"] = max_clients;
    JsonArray list = rfc["clients"].to<JsonArray>();
    rfc2217_info_t info[4];
    const int n = rfc2217_info(info, 4);
    for (int i = 0; i < n; i++) {
        JsonObject c = list.add<JsonObject>();
        c["ip"] = info[i].ip;
        c["port"] = info[i].port;
        c["connected_s"] = info[i].connected_s;
        c["idle_s"] = info[i].idle_s;
        c["rx"] = info[i].rx;
        c["tx"] = info[i].tx;
        c["commands"] = info[i].commands;
        c["refused"] = info[i].refused;
    }

    return send_json(req, doc);
}

// ---------------------------------------------------------------------------
// Public API (/api/v1) for external integrations, e.g. Home Assistant.
// Bearer-authenticated with the API token; independent of the admin session.
// GET /api/v1/events (below, with /ws) pushes the state as it changes.
// ---------------------------------------------------------------------------

// The API's version: "api_version" in /api/v1/info and in /api/v1/events' "hello", "version" in the
// _svsbridge._tcp TXT. New routes, keys and event types keep it.
static const int API_VERSION = 1;

// Identity and capabilities, read once by the integration's config flow.
static esp_err_t api_info_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    JsonDocument doc;
    doc["id"] = wifi_manager::device_id();  // stable unique_id
    doc["name"] = "SVS Bridge";
    doc["model"] = "SVS Bridge (ESP32-S3)";
    doc["manufacturer"] = "SVS Bridge";
    doc["sw_version"] = app->version;
    doc["hostname"] = CONFIG_SVS_HOSTNAME ".local";
    doc["api_version"] = API_VERSION;
    return send_json(req, doc);
}

// Live state: polled by the integration (GET /api/v1/state), or pushed (/api/v1/events).
static void add_api_state(JsonObject state)
{
    add_svs_info(state["svs"].to<JsonObject>());
    JsonObject bridge = state["bridge"].to<JsonObject>();
    // The running firmware version, so the integration always shows the current
    // one (not just what it read at setup) and can detect a newer release.
    bridge["sw_version"] = esp_app_get_description()->version;
    bridge["rssi"] = wifi_manager::sta_rssi();
    bridge["uptime_s"] = esp_timer_get_time() / 1000000;
}

static esp_err_t api_state_get(httpd_req_t *req)
{
    JsonDocument doc;
    add_api_state(doc.to<JsonObject>());
    return send_json(req, doc);
}

// The current API token, for the web UI to show and copy (admin only).
static esp_err_t api_token_get(httpd_req_t *req)
{
    JsonDocument doc;
    doc["token"] = auth::api_token();
    return send_json(req, doc);
}

static esp_err_t api_token_regenerate_post(httpd_req_t *req)
{
    JsonDocument doc;
    doc["token"] = auth::regenerate_api_token();
    return send_json(req, doc);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    std::string out = wifi_manager::scan_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, out.data(), out.size());
}

static esp_err_t cert_get(httpd_req_t *req)
{
    const std::string &pem = tls_cert::cert_pem();
    httpd_resp_set_type(req, "application/x-pem-file");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"" CONFIG_SVS_HOSTNAME ".crt\"");
    return httpd_resp_send(req, pem.data(), pem.size());
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    JsonDocument body;
    if (!read_json_body(req, body)) {
        return ESP_OK;
    }
    std::string ssid = body["ssid"] | "";
    std::string password = body["password"] | "";
    if (wifi_manager::set_credentials(ssid, password) != ESP_OK) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "Invalid SSID or password");
    }
    return send_ok(req);
}

// Writes a firmware image into the inactive OTA slot. `read(buf, want)` fills up
// to `want` bytes and returns the count, 0 at end of stream, or <0 on error;
// `total` is the whole image size. The image is checked to be an svs_bridge
// build for this chip before any flash is erased; esp_ota_end() then verifies
// the full checksum. On success the boot partition is switched (caller reboots).
// Returns ESP_OK, or sets *err_msg and returns a failure code.
static esp_err_t ota_apply(size_t total, const std::function<int(uint8_t *, size_t)> &read,
                           std::string &err_msg,
                           const std::function<void(size_t, size_t)> *progress = nullptr)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == nullptr) {
        err_msg = "No OTA partition";
        return ESP_FAIL;
    }
    if (total == 0 || total > target->size) {
        err_msg = "Missing or too large image";
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t header_len =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    std::unique_ptr<uint8_t[]> buf(new uint8_t[OTA_CHUNK]);
    esp_ota_handle_t handle = 0;
    bool started = false;
    size_t remaining = total;
    size_t filled = 0;  // bytes in buf not yet written

    ESP_LOGI(TAG, "OTA: writing %u bytes into %s", (unsigned)total, target->label);

    while (remaining > 0) {
        size_t want = OTA_CHUNK - filled;
        if (want > remaining) {
            want = remaining;
        }
        int r = read(buf.get() + filled, want);
        if (r == 0) {
            if (started) esp_ota_abort(handle);
            err_msg = "Transfer ended early";
            return ESP_FAIL;
        }
        if (r < 0) {
            if (started) esp_ota_abort(handle);
            err_msg = "Connection lost";
            return ESP_FAIL;
        }
        filled += r;
        remaining -= r;

        if (!started) {
            // Wait until the headers are in, then validate before erasing flash
            if (filled < header_len && remaining > 0) {
                continue;
            }
            if (filled < header_len) {
                err_msg = "Image too small";
                return ESP_FAIL;
            }
            const auto *img = (const esp_image_header_t *)buf.get();
            const auto *desc = (const esp_app_desc_t *)(buf.get() + sizeof(esp_image_header_t) +
                                                        sizeof(esp_image_segment_header_t));
            if (img->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
                err_msg = "Not an ESP-IDF app image";
                return ESP_FAIL;
            }
            if (img->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
                err_msg = "Image is for a different chip";
                return ESP_FAIL;
            }
            const esp_app_desc_t *running = esp_app_get_description();
            if (strncmp(desc->project_name, running->project_name, sizeof(desc->project_name)) != 0) {
                err_msg = "Image is not an svs_bridge firmware";
                return ESP_FAIL;
            }
            ESP_LOGI(TAG, "OTA: new version %.32s (running %s)", desc->version, running->version);

            esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
            if (err != ESP_OK) {
                err_msg = esp_err_to_name(err);
                return err;
            }
            started = true;
        }

        // Write when the buffer is full or the transfer is complete
        if (filled == OTA_CHUNK || remaining == 0) {
            esp_err_t err = esp_ota_write(handle, buf.get(), filled);
            if (err != ESP_OK) {
                esp_ota_abort(handle);
                err_msg = esp_err_to_name(err);
                return err;
            }
            filled = 0;
            if (progress && *progress) {
                (*progress)(total - remaining, total);
            }
        }
    }

    esp_err_t err = esp_ota_end(handle);
    if (err != ESP_OK) {
        err_msg = err == ESP_ERR_OTA_VALIDATE_FAILED ? "Image is corrupted" : esp_err_to_name(err);
        return err;
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        err_msg = esp_err_to_name(err);
        return err;
    }
    ESP_LOGI(TAG, "OTA: done, boot partition set to %s", target->label);
    return ESP_OK;
}

// Receives an uploaded firmware image and applies it to the inactive OTA slot.
static esp_err_t ota_post(httpd_req_t *req)
{
    if (svs_flasher::busy() || svs_settings::busy()) {
        return send_busy(req);
    }
    if (req->content_len == 0) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "Missing image");
    }
    auto read = [&](uint8_t *b, size_t want) -> int {
        int r;
        do {
            r = httpd_req_recv(req, (char *)b, want);
        } while (r == HTTPD_SOCK_ERR_TIMEOUT);
        return r;  // <= 0 on error/close
    };
    std::string err;
    if (ota_apply(req->content_len, read, err) != ESP_OK) {
        return send_error(req, HTTPD_400_BAD_REQUEST, err.c_str());
    }
    send_ok(req);
    schedule_reboot();
    return ESP_OK;
}

// Lists the bridge's own GitHub releases (with the running version) so the web
// UI can offer a one-click update.
static esp_err_t releases_get(httpd_req_t *req)
{
    std::vector<bridge_fw_repo::Release> releases;
    std::string err;
    if (bridge_fw_repo::list(releases, &err) != ESP_OK) {
        JsonDocument doc;
        doc["error"] = err;
        httpd_resp_set_status(req, "502 Bad Gateway");
        return send_json(req, doc);
    }
    JsonDocument doc;
    doc["running"] = esp_app_get_description()->version;
    JsonArray arr = doc["releases"].to<JsonArray>();
    for (const auto &r : releases) {
        JsonObject o = arr.add<JsonObject>();
        o["tag"] = r.tag;
        o["name"] = r.name;
        o["notes_url"] = r.notes_url;
        o["prerelease"] = r.prerelease;
        o["size"] = r.size;
    }
    return send_json(req, doc);
}

// Opens `url`, following redirects (GitHub's asset URL 302-redirects to its CDN
// on another host, with a very long signed URL). The only change needed over a
// plain client is a bigger tx buffer for that long request line. On success
// returns a client positioned at the 200 response via *out (caller closes it).
static esp_err_t open_following_redirects(const std::string &url, esp_http_client_handle_t *out,
                                          std::string &err)
{
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 20000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;  // the signed CDN URL makes a long request line
    config.user_agent = "svs-bridge";
    config.max_redirection_count = 5;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        err = "Out of memory";
        return ESP_ERR_NO_MEM;
    }

    for (int hop = 0; hop < 6; hop++) {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Could not reach GitHub (hop %d, errno %d)", hop,
                     esp_http_client_get_errno(client));
            err = msg;
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            esp_http_client_set_redirection(client);  // point the client at the Location
            esp_http_client_close(client);            // before re-opening the same client
            continue;
        }
        if (status != 200) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            char msg[48];
            snprintf(msg, sizeof(msg), "Download failed (HTTP %d)", status);
            err = msg;
            return ESP_FAIL;
        }
        *out = client;
        return ESP_OK;
    }
    esp_http_client_cleanup(client);
    err = "Too many redirects";
    return ESP_FAIL;
}

// Downloads a chosen GitHub release's firmware and applies it over the air.
static esp_err_t ota_github_post(httpd_req_t *req)
{
    if (svs_flasher::busy() || svs_settings::busy()) {
        return send_busy(req);
    }
    std::string body;
    if (!read_body(req, MAX_JSON_BODY, body)) {
        return ESP_FAIL;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }
    std::string tag = doc["tag"] | "";
    std::string url;
    size_t size = 0;
    if (tag.empty() || !bridge_fw_repo::resolve(tag, url, size)) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "Unknown release; refresh the list first");
    }

    esp_http_client_handle_t client = nullptr;
    std::string err;
    ESP_LOGI(TAG, "OTA: downloading %s (%u B)", tag.c_str(), (unsigned)size);
    if (open_following_redirects(url, &client, err) != ESP_OK) {
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, err.c_str());
    }

    // The bridge downloads server-side, so the browser cannot see the transfer.
    // Stream progress to it as chunked text: a percentage per line, then "done"
    // or "err:<message>".
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    int last_pct = -1;
    std::function<void(size_t, size_t)> progress = [&](size_t done, size_t total) {
        int pct = total ? (int)(100 * done / total) : 0;
        if (pct != last_pct) {
            last_pct = pct;
            char line[8];
            int n = snprintf(line, sizeof(line), "%d\n", pct);
            httpd_resp_send_chunk(req, line, n);  // send errors are ignored; the OTA continues
        }
    };

    auto read = [&](uint8_t *b, size_t want) -> int {
        return esp_http_client_read(client, (char *)b, want);
    };
    esp_err_t rc = ota_apply(size, read, err, &progress);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (rc != ESP_OK) {
        std::string line = "err:" + err + "\n";
        httpd_resp_send_chunk(req, line.data(), line.size());
        httpd_resp_send_chunk(req, nullptr, 0);
        return ESP_OK;
    }
    httpd_resp_send_chunk(req, "done\n", 5);
    httpd_resp_send_chunk(req, nullptr, 0);
    schedule_reboot();
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (svs_flasher::busy() || svs_settings::busy()) {
        return send_busy(req);
    }
    send_ok(req);
    schedule_reboot();
    return ESP_OK;
}

// Erases all settings (WiFi, TLS certificate, ...) and reboots into setup mode
static esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (svs_flasher::busy() || svs_settings::busy()) {
        return send_busy(req);
    }
    ESP_LOGW(TAG, "Factory reset requested");
    send_ok(req);
    s_erase_before_reboot = true;
    schedule_reboot();
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// SVS
// ---------------------------------------------------------------------------

static const char *task_name(svs_flasher::Task task)
{
    switch (task) {
    case svs_flasher::Task::Checking: return "checking";
    case svs_flasher::Task::Flashing: return "flashing";
    case svs_flasher::Task::Probing: return "probing";
    case svs_flasher::Task::Previewing: return "previewing";
    case svs_flasher::Task::Idle:
    default: return "idle";
    }
}

// SVS info plus the firmware update state:
// {"connected", "firmware", "current_input", "total_inputs",
//  "update": {"task", "phase", "progress",
//             "device": {"checked", "compatible", "summary", "problem", "app_space"},
//             "image": {"staged", "source", "size", "sha256"},
//             "can_flash", "blocker", "result", "result_ok", "result_of"}}
// (also pushed over /ws)
static void add_svs_state(JsonObject svs)
{
    add_svs_info(svs);

    svs_flasher::Status st = svs_flasher::status();
    JsonObject update = svs["update"].to<JsonObject>();
    update["task"] = task_name(st.task);
    update["phase"] = st.phase;
    update["progress"] = st.progress;

    JsonObject device = update["device"].to<JsonObject>();
    device["checked"] = st.device.checked;
    device["compatible"] = st.device.compatible;
    device["vector"] = st.device.vector;
    device["vector_verified"] = st.device.vector_verified;
    device["summary"] = st.device.summary;
    device["problem"] = st.device.problem;
    device["app_space"] = st.device.app_space;

    JsonObject image = update["image"].to<JsonObject>();
    image["staged"] = st.image.staged;
    image["source"] = st.image.source;
    image["size"] = st.image.size;
    image["sha256"] = st.image.sha256;

    update["can_flash"] = st.can_flash;
    update["blocker"] = st.blocker;
    update["result"] = st.result;
    update["result_ok"] = st.result_ok;
    update["result_of"] = task_name(st.result_of);
}

static esp_err_t svs_get(httpd_req_t *req)
{
    JsonDocument doc;
    add_svs_state(doc.to<JsonObject>());
    return send_json(req, doc);
}

static const size_t LOG_BATCH = 100;

// Up to LOG_BATCH entries after `after` into doc (also pushed over /ws). Returns how many.
static size_t add_log_entries(JsonDocument &doc, uint32_t after, uint32_t *last_seq)
{
    doc["now"] = esp_timer_get_time() / 1000;
    JsonArray entries = doc["entries"].to<JsonArray>();
    std::vector<svs_usb::LogEntry> list = svs_usb::log_since(after, LOG_BATCH);
    for (const auto &e : list) {
        JsonObject o = entries.add<JsonObject>();
        o["seq"] = e.seq;
        o["t"] = e.ms;
        o["d"] = std::string(1, e.dir);
        o["s"] = e.text;
        *last_seq = e.seq;
    }
    return list.size();
}

// Traffic with the SVS since entry `after` (query string):
// {"now": uptime ms, "entries": [{"seq", "t" (uptime ms), "d" ('<' '>' '*'), "s"}]}
static esp_err_t svs_log_get(httpd_req_t *req)
{
    uint32_t after = 0;
    char query[48];
    char value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "after", value, sizeof(value)) == ESP_OK) {
        after = strtoul(value, nullptr, 10);
    }

    JsonDocument doc;
    add_log_entries(doc, after, &after);
    return send_json(req, doc);
}

// A line for the SVS, sent as typed plus the configured line ending (also from
// /ws). Returns 0 once sent, else the HTTP status to refuse it with and why.
static int svs_send_command(const std::string &command, std::string &error)
{
    if (svs_settings::busy()) {
        error = busy_message();
        return 409;
    }
    if (command.empty() || command.size() > 128) {
        error = "Enter a command (up to 128 characters)";
        return 400;
    }
    esp_err_t err = svs_usb::send(command);
    if (err == ESP_ERR_NOT_ALLOWED) {
        error = busy_message();
        return 409;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        error = "The SVS is not connected";
        return 400;
    }
    if (err != ESP_OK) {
        error = esp_err_to_name(err);
        return 500;
    }
    return 0;
}

// Body: {"command": "SVS_Input_Up"}
static esp_err_t svs_send_post(httpd_req_t *req)
{
    JsonDocument body;
    if (!read_json_body(req, body)) {
        return ESP_OK;
    }
    std::string error;
    switch (svs_send_command(body["command"] | "", error)) {
    case 0:
        return send_ok(req);
    case 409:
        return send_busy(req);
    case 400:
        return send_error(req, HTTPD_400_BAD_REQUEST, error.c_str());
    default:
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, error.c_str());
    }
}

// Restarts the SVS so it reports its firmware version and inputs again
static esp_err_t svs_restart_post(httpd_req_t *req)
{
    if (svs_settings::busy()) {
        return send_busy(req);
    }
    esp_err_t err = svs_usb::restart_svs();
    if (err == ESP_ERR_NOT_ALLOWED) {
        return send_busy(req);
    }
    if (err != ESP_OK) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "The SVS is not connected");
    }
    return send_ok(req);
}

// Validates the vector-bootloader patch against the chip; writes nothing
static esp_err_t svs_preview_post(httpd_req_t *req)
{
    if (svs_flasher::busy() || svs_settings::busy()) {
        return send_busy(req);
    }
    if (svs_flasher::start_preview() != ESP_OK) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "Check the SVS first");
    }
    return svs_get(req);
}

// Bootloader diagnostics (restarts the SVS several times; writes nothing)
static esp_err_t svs_probe_post(httpd_req_t *req)
{
    if (svs_flasher::busy() || svs_settings::busy()) {
        return send_busy(req);
    }
    if (svs_flasher::start_probe() != ESP_OK) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "The SVS is not connected");
    }
    return svs_get(req);
}

// Step 1: identify the SVS bootloader (restarts the SVS)
static esp_err_t svs_check_post(httpd_req_t *req)
{
    if (svs_flasher::busy() || svs_settings::busy()) {
        return send_busy(req);
    }
    if (svs_flasher::start_check() != ESP_OK) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "The SVS is not connected");
    }
    return svs_get(req);
}

static esp_err_t svs_releases_get(httpd_req_t *req)
{
    std::vector<svs_fw_repo::Release> releases;
    std::string error;
    if (svs_fw_repo::list(releases, &error) != ESP_OK) {
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, error.c_str());
    }
    JsonDocument doc;
    JsonArray list = doc.to<JsonArray>();
    for (const auto &r : releases) {
        JsonObject o = list.add<JsonObject>();
        o["name"] = r.name;
        o["version"] = r.version;
        o["beta"] = r.beta;
        o["size"] = r.size;
    }
    return send_json(req, doc);
}

static esp_err_t stage_and_reply(httpd_req_t *req, const std::string &hex, const std::string &source)
{
    std::string error;
    esp_err_t err = svs_flasher::stage_hex(hex.data(), hex.size(), source, &error);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_busy(req);
    }
    if (err != ESP_OK) {
        return send_error(req, HTTPD_400_BAD_REQUEST, error.c_str());
    }
    return svs_get(req);
}

// Body: the .hex file itself; optional X-File-Name header for display
static esp_err_t svs_firmware_upload_post(httpd_req_t *req)
{
    if (svs_flasher::busy() || svs_settings::busy()) {
        return send_busy(req);
    }
    std::string hex;
    if (!read_body(req, MAX_SVS_HEX, hex)) {
        svs_flasher::clear();
        return ESP_OK;
    }
    char name[64] = "uploaded file";
    httpd_req_get_hdr_value_str(req, "X-File-Name", name, sizeof(name));
    return stage_and_reply(req, hex, std::string(name) + " (uploaded)");
}

// Body: {"name": "SVS_FW_1.21.hex"} from the official releases list
static esp_err_t svs_firmware_official_post(httpd_req_t *req)
{
    JsonDocument body;
    if (!read_json_body(req, body)) {
        return ESP_OK;
    }
    std::string name = body["name"] | "";
    std::string hex, error;
    if (svs_flasher::busy() || svs_settings::busy()) {
        return send_busy(req);
    }
    if (svs_fw_repo::download(name, hex, &error) != ESP_OK) {
        svs_flasher::clear();  // never leave an earlier choice staged
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, error.c_str());
    }
    return stage_and_reply(req, hex, name + " (official repository)");
}

static esp_err_t svs_flash_post(httpd_req_t *req)
{
    if (svs_flasher::busy() || svs_settings::busy()) {
        return send_busy(req);
    }
    if (svs_flasher::start_flash() != ESP_OK) {
        std::string blocker = svs_flasher::status().blocker;
        return send_error(req, HTTPD_400_BAD_REQUEST,
                          blocker.empty() ? "Cannot flash right now" : blocker.c_str());
    }
    return svs_get(req);
}

// ---------------------------------------------------------------------------
// SVS settings (stored in the SVS) and layout (kept on the bridge)
// ---------------------------------------------------------------------------

static const char *settings_task_name(svs_settings::Task t)
{
    switch (t) {
    case svs_settings::Task::Reading: return "reading";
    case svs_settings::Task::Writing: return "writing";
    default: return "idle";
    }
}

// {"task", "phase", "progress", "result", "result_ok", "result_of", "blocker",
//  "snapshot": null | {"seq", "age_s", "firmware", "inputs", "ir_slots",
//     "transcoders": {"rgb_to_ypbpr", "ypbpr_to_rgb"},
//     "v3": [{"scart", "vga"}],
//     "settings": [{"auto_profile", "rgsb", "sync_bypass", "rgb_to_ypbpr", "ypbpr_to_rgb",
//                   "ir": [[address, command], ...]}]}}
static esp_err_t send_svs_config(httpd_req_t *req)
{
    svs_settings::Status st = svs_settings::status();
    JsonDocument doc;
    doc["task"] = settings_task_name(st.task);
    doc["phase"] = st.phase;
    doc["progress"] = st.progress;
    doc["result"] = st.result;
    doc["result_ok"] = st.result_ok;
    doc["result_of"] = settings_task_name(st.result_of);
    doc["blocker"] = st.blocker;
    const svs_settings::Snapshot &sn = st.snapshot;
    if (!sn.valid) {
        doc["snapshot"] = nullptr;
        return send_json(req, doc);
    }
    JsonObject o = doc["snapshot"].to<JsonObject>();
    o["seq"] = sn.seq;
    o["age_s"] = sn.age_ms / 1000;
    o["firmware"] = sn.firmware;
    o["inputs"] = sn.inputs;
    o["ir_slots"] = sn.ir_slots;
    JsonObject tx = o["transcoders"].to<JsonObject>();
    tx["rgb_to_ypbpr"] = sn.hardware.tx_rgb_to_ypbpr;
    tx["ypbpr_to_rgb"] = sn.hardware.tx_ypbpr_to_rgb;
    JsonArray v3 = o["v3"].to<JsonArray>();
    JsonArray settings = o["settings"].to<JsonArray>();
    for (int i = 0; i < sn.inputs; i++) {
        JsonObject m = v3.add<JsonObject>();
        m["scart"] = (bool)sn.hardware.scart_v3[i];
        m["vga"] = (bool)sn.hardware.vga_v3[i];
        const svs_config::InputSettings &in = sn.settings[i];
        JsonObject e = settings.add<JsonObject>();
        e["auto_profile"] = in.auto_profile;
        e["rgsb"] = in.rgsb;
        e["sync_bypass"] = in.sync_bypass;
        e["rgb_to_ypbpr"] = in.rgb_to_ypbpr;
        e["ypbpr_to_rgb"] = in.ypbpr_to_rgb;
        JsonArray ir = e["ir"].to<JsonArray>();
        for (const auto &c : in.ir) {
            JsonArray pair = ir.add<JsonArray>();
            pair.add(c.address);
            pair.add(c.command);
        }
    }
    return send_json(req, doc);
}

static esp_err_t svs_config_get(httpd_req_t *req)
{
    return send_svs_config(req);
}

static esp_err_t settings_error(httpd_req_t *req, esp_err_t err, const std::string &error)
{
    JsonDocument doc;
    doc["error"] = error.empty() ? esp_err_to_name(err) : error;
    httpd_resp_set_status(req, err == ESP_ERR_INVALID_ARG ? "400 Bad Request" : "409 Conflict");
    return send_json(req, doc);
}

// Reads the settings from the SVS, in the background (poll GET)
static esp_err_t svs_config_read_post(httpd_req_t *req)
{
    if (svs_flasher::busy()) {
        return send_busy(req);
    }
    std::string error;
    esp_err_t err = svs_settings::start_read(&error);
    if (err != ESP_OK) {
        return settings_error(req, err, error);
    }
    return send_svs_config(req);
}

// Body: {"settings": [one entry per input, as in GET]}; saved in the background
static esp_err_t svs_config_write_post(httpd_req_t *req)
{
    if (svs_flasher::busy()) {
        return send_busy(req);
    }
    std::string body_text;
    if (!read_body(req, MAX_SETTINGS_BODY, body_text)) {
        return ESP_OK;
    }
    JsonDocument body;
    if (deserializeJson(body, body_text) != DeserializationError::Ok) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }
    std::vector<svs_config::InputSettings> want;
    for (JsonObjectConst e : body["settings"].as<JsonArrayConst>()) {
        svs_config::InputSettings in;
        in.auto_profile = e["auto_profile"] | true;
        in.rgsb = e["rgsb"] | false;
        in.sync_bypass = e["sync_bypass"] | false;
        in.rgb_to_ypbpr = e["rgb_to_ypbpr"] | false;
        in.ypbpr_to_rgb = e["ypbpr_to_rgb"] | false;
        for (JsonArrayConst pair : e["ir"].as<JsonArrayConst>()) {
            int a = pair[0] | -1, c = pair[1] | -1;
            // 0xFF marks the end of a list, so it cannot be an address
            if (pair.size() != 2 || a < 0 || a >= 0xFF || c < 0 || c > 0xFF) {
                return send_error(req, HTTPD_400_BAD_REQUEST, "Invalid IR code");
            }
            in.ir.push_back({(uint8_t)a, (uint8_t)c});
        }
        want.push_back(in);
    }
    std::string error;
    esp_err_t err = svs_settings::start_write(want, &error);
    if (err != ESP_OK) {
        return settings_error(req, err, error);
    }
    return send_svs_config(req);
}

static esp_err_t send_layout(httpd_req_t *req)
{
    std::string json = svs_settings::layout_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json.data(), json.size());
}

static esp_err_t svs_layout_get(httpd_req_t *req)
{
    return send_layout(req);
}

// Body: {"inputs": [{"kind", "name"}], "outputs": [{"kind", "name"}]}
static esp_err_t svs_layout_post(httpd_req_t *req)
{
    std::string body;
    if (!read_body(req, MAX_LAYOUT_BODY, body)) {
        return ESP_OK;
    }
    std::string error;
    if (svs_settings::set_layout_json(body, &error) != ESP_OK) {
        return send_error(req, HTTPD_400_BAD_REQUEST, error.c_str());
    }
    return send_layout(req);
}

// ---------------------------------------------------------------------------
// Cruller (the RT4K bridge the active input is reported to)
// ---------------------------------------------------------------------------

// {"selected": "<id>" | null,
//  "found": [{"id", "name", "instance", "host", "ip", "port", "version", "selected"}],
//  "last": {"ok", "http_status", "error", "paired_with", "input", "ago_s", "count"} | null}
static esp_err_t send_cruller_state(httpd_req_t *req)
{
    cruller::State st = cruller::state();
    int64_t now_ms = esp_timer_get_time() / 1000;
    JsonDocument doc;
    if (st.selected_id.empty()) {
        doc["selected"] = nullptr;
    } else {
        doc["selected"] = st.selected_id;
    }
    JsonArray found = doc["found"].to<JsonArray>();
    for (const auto &f : st.found) {
        JsonObject o = found.add<JsonObject>();
        o["id"] = f.id;
        o["name"] = f.name;
        o["instance"] = f.instance;
        o["host"] = f.host.empty() ? "" : f.host + ".local";
        o["ip"] = f.ip;
        o["port"] = f.port;
        o["version"] = f.version;
        o["selected"] = f.id == st.selected_id;
    }
    if (!st.last.attempted) {
        doc["last"] = nullptr;
    } else {
        JsonObject last = doc["last"].to<JsonObject>();
        last["ok"] = st.last.ok;
        last["http_status"] = st.last.http_status;
        last["error"] = st.last.error;
        last["paired_with"] = st.last.paired_with;
        last["input"] = st.last.input;
        last["ago_s"] = (now_ms - st.last.at_ms) / 1000;
        last["count"] = st.last.count;
    }
    return send_json(req, doc);
}

static esp_err_t cruller_get(httpd_req_t *req)
{
    return send_cruller_state(req);
}

// Browses the network again (about 2 s), then replies like GET /device/cruller
static esp_err_t cruller_scan_post(httpd_req_t *req)
{
    cruller::scan();
    return send_cruller_state(req);
}

// Body: {"id": "<Cruller id>"}; "" stops reporting
static esp_err_t cruller_select_post(httpd_req_t *req)
{
    JsonDocument body;
    if (!read_json_body(req, body)) {
        return ESP_OK;
    }
    std::string id = body["id"] | "";
    esp_err_t err = cruller::select(id);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "Invalid Cruller id");
    }
    if (err != ESP_OK) {
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    return send_cruller_state(req);
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

static std::string session_token(httpd_req_t *req)
{
    char buf[80];
    size_t len = sizeof(buf);
    if (httpd_req_get_cookie_val(req, SESSION_COOKIE, buf, &len) != ESP_OK) {
        return "";
    }
    return buf;
}

static bool authenticated(httpd_req_t *req)
{
    std::string token = session_token(req);
    return !token.empty() && auth::session_valid(token);
}

// True if the request carries a valid "Authorization: Bearer <api token>".
// The token of an "Authorization: Bearer <token>" header ("" if none)
static std::string bearer_token(httpd_req_t *req)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 0 || len > 128) {
        return "";
    }
    std::string header(len, '\0');
    if (httpd_req_get_hdr_value_str(req, "Authorization", &header[0], len + 1) != ESP_OK) {
        return "";
    }
    const std::string prefix = "Bearer ";
    if (header.rfind(prefix, 0) != 0) {
        return "";
    }
    return header.substr(prefix.size());
}

static bool api_authorized(httpd_req_t *req)
{
    std::string token = bearer_token(req);
    return !token.empty() && auth::api_token_valid(token);
}

// Sends doc along with a Set-Cookie header for the session (empty = clear it).
// The cookie is Secure over HTTPS; the setup portal only has plain HTTP.
static esp_err_t send_json_with_session(httpd_req_t *req, const JsonDocument &doc,
                                        const std::string &token)
{
    std::string cookie = std::string(SESSION_COOKIE) + "=" + token +
                         "; Path=/; HttpOnly; SameSite=Strict";
    if (token.empty()) {
        cookie += "; Max-Age=0";
    }
    if (req->handle == s_https_server) {
        cookie += "; Secure";
    }
    httpd_resp_set_hdr(req, "Set-Cookie", cookie.c_str());
    return send_json(req, doc);  // cookie must stay alive until this returns
}

static esp_err_t send_status_json(httpd_req_t *req, const char *status, const JsonDocument &doc)
{
    httpd_resp_set_status(req, status);
    return send_json(req, doc);
}

static esp_err_t auth_get(httpd_req_t *req)
{
    JsonDocument doc;
    doc["password_set"] = auth::password_set();
    doc["authenticated"] = authenticated(req);
    return send_json(req, doc);
}

// First-time setup only: once a password exists it can only be changed by an
// authenticated admin, never through this endpoint (the setup portal is on an
// open network and comes back whenever the WiFi connection is lost).
static esp_err_t setup_password_post(httpd_req_t *req)
{
    if (auth::password_set()) {
        JsonDocument doc;
        doc["error"] = "Password already set";
        return send_status_json(req, "409 Conflict", doc);
    }
    JsonDocument body;
    if (!read_json_body(req, body)) {
        return ESP_OK;
    }
    std::string password = body["password"] | "";
    esp_err_t err = auth::set_password(password);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "Password must be 8 to 64 characters");
    }
    if (err != ESP_OK) {
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    JsonDocument doc;
    doc["ok"] = true;
    return send_json_with_session(req, doc, auth::create_session());
}

static esp_err_t login_post(httpd_req_t *req)
{
    JsonDocument body;
    if (!read_json_body(req, body)) {
        return ESP_OK;
    }
    std::string password = body["password"] | "";
    int retry_after = 0;
    JsonDocument doc;
    switch (auth::check_password(password, &retry_after)) {
    case auth::LoginResult::Ok:
        doc["ok"] = true;
        return send_json_with_session(req, doc, auth::create_session());
    case auth::LoginResult::Locked:
        doc["error"] = "Too many failed attempts";
        doc["retry_after"] = retry_after;
        return send_status_json(req, "429 Too Many Requests", doc);
    case auth::LoginResult::WrongPassword:
    default:
        doc["error"] = "Wrong password";
        return send_status_json(req, "401 Unauthorized", doc);
    }
}

static esp_err_t logout_post(httpd_req_t *req)
{
    std::string token = session_token(req);
    if (!token.empty()) {
        auth::end_session(token);
    }
    JsonDocument doc;
    doc["ok"] = true;
    return send_json_with_session(req, doc, "");
}

// ---------------------------------------------------------------------------
// Live updates (/ws)
//
// Each open web UI keeps a WebSocket: the bridge pushes the SVS's traffic log
// and status as they change, and takes the input commands on it, with no
// request (and no TLS handshake) per command. Without it the page polls.
// Text frames, JSON:
//   page:   {"type": "hello", "after": N}  ready: the log after entry N, and the status
//           {"type": "send", "id": n, "command": "SVS_Input_Up"}  as POST /device/svs/send
//           {"type": "ping"}  now and then; the answer shows the socket still works
//   bridge: {"type": "svs", "svs": {as GET /device/svs}, "cfg": {"task", "seq", "blocker"}}
//             whenever any of it changes (cfg changing: GET /device/svs/config again)
//           {"type": "log", "now", "entries": [as GET /device/svs/log]}
//           {"type": "reply", "id": n, "ok": true} or {"type": "reply", "id": n, "status", "error"}
//           {"type": "pong"}
//           {"type": "auth"}  the session ended (logout, expiry); the socket closes
// The session cookie is checked before the handshake, then before each push.
//
// GET /api/v1/events is the same machinery for Home Assistant and scripts, with the API token
// (Authorization: Bearer) instead of a session, and Cruller's /api/v1/events contract:
//   bridge: {"type": "hello", "api_version": 1, "types": [...], "subscribed": [...]}  first
//           {"type": "state", "state": {as GET /api/v1/state}}  if it asked for "state" (?types=,
//             "state" by default): at once, when the "svs" part changes, and at least every minute
//           {"type": "auth"}  the API token was regenerated; the socket closes
// What the client sends is reserved (messages a later v1 announces in "hello"): ignored for now.
// ---------------------------------------------------------------------------

static const int HTTPS_MAX_SOCKETS = 7;  // each open page holds one for its WebSocket
static const size_t MAX_WS_FRAME = 512;
static const int WS_TICK_MS = 500;  // for what changes without a log line (flash progress)
static const int EVENTS_MAX = 2;  // /api/v1/events sockets at once (a new one closes the oldest)
static const int64_t EVENTS_EVERY_US = 60 * 1000000LL;  // an unchanged state again this often

struct WsClient {
    bool events = false;     // an /api/v1/events client, not a page
    std::string token;       // a page's session; an events client's API token
    bool ready = false;      // page: "hello" received; events client: its "hello" sent
    uint32_t log_after = 0;  // the newest log entry it has
    std::string svs;         // page: the "svs" message it has; events client: its last state's "svs"
    uint32_t types = 0;      // events client: its api_events types
    int64_t sent_us = 0;     // events client: when its last state went
    int64_t opened_us = 0;   // events client: when it connected (the oldest makes room)
};

static std::atomic<int> s_ws_clients{0};
static std::atomic<bool> s_ws_flush_queued{false};
static esp_timer_handle_t s_ws_timer = nullptr;

static void ws_client_free(void *ctx)
{
    delete static_cast<WsClient *>(ctx);
    s_ws_clients--;
}

static void ws_flush(void *arg);

// From any task: have the server's task push what is new (one run pending at most)
static void ws_notify()
{
    if (s_ws_clients.load() > 0 && !s_ws_flush_queued.exchange(true) &&
        httpd_queue_work(s_https_server, ws_flush, nullptr) != ESP_OK) {
        s_ws_flush_queued = false;
    }
}

static void on_ws_tick(void *arg)
{
    ws_notify();
}

static std::string ws_svs_message()
{
    JsonDocument doc;
    doc["type"] = "svs";
    add_svs_state(doc["svs"].to<JsonObject>());
    svs_settings::Status st = svs_settings::status();
    JsonObject cfg = doc["cfg"].to<JsonObject>();
    cfg["task"] = settings_task_name(st.task);
    cfg["seq"] = st.snapshot.valid ? st.snapshot.seq : 0;
    cfg["blocker"] = st.blocker;
    std::string out;
    serializeJson(doc, out);
    return out;
}

static esp_err_t ws_send_text(int fd, const std::string &text)
{
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)text.data();
    frame.len = text.size();
    return httpd_ws_send_frame_async(s_https_server, fd, &frame);
}

// An /api/v1/events client, in the server's task: its "hello" first, then (if it asked for "state")
// the state as soon as its "svs" part changes (svs_key: that part now, built once per flush), and at
// least every EVENTS_EVERY_US. Not rssi or uptime alone: they change all the time.
static esp_err_t events_flush(int fd, WsClient *c, std::string &svs_key)
{
    if (!auth::api_token_valid(c->token)) {  // regenerated in the web UI
        ws_send_text(fd, "{\"type\":\"auth\"}");
        return ESP_FAIL;
    }
    if (!c->ready) {
        c->ready = true;
        const std::string hello = "{\"type\":\"hello\",\"api_version\":" + std::to_string(API_VERSION) +
                                  ",\"types\":" + api_events::names_json(api_events::ALL) +
                                  ",\"subscribed\":" + api_events::names_json(c->types) + "}";
        esp_err_t err = ws_send_text(fd, hello);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (!(c->types & api_events::STATE)) {
        return ESP_OK;
    }
    if (svs_key.empty()) {
        JsonDocument doc;
        add_svs_info(doc.to<JsonObject>());
        serializeJson(doc, svs_key);
    }
    const int64_t now = esp_timer_get_time();
    if (svs_key == c->svs && now - c->sent_us < EVENTS_EVERY_US) {
        return ESP_OK;
    }
    c->svs = svs_key;
    c->sent_us = now;
    JsonDocument doc;
    doc["type"] = "state";
    add_api_state(doc["state"].to<JsonObject>());
    std::string out;
    serializeJson(doc, out);
    return ws_send_text(fd, out);
}

// In the server's task (httpd_queue_work): sends each page and events client what it has not seen
static void ws_flush(void *arg)
{
    s_ws_flush_queued = false;
    int fds[HTTPS_MAX_SOCKETS];
    size_t n = HTTPS_MAX_SOCKETS;
    if (httpd_get_client_list(s_https_server, &n, fds) != ESP_OK) {
        return;
    }
    std::string svs;      // built once, for the pages that need it
    std::string svs_key;  // likewise, for the events clients
    bool more = false;
    for (size_t i = 0; i < n; i++) {
        const int fd = fds[i];
        if (httpd_ws_get_fd_info(s_https_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
            continue;
        }
        auto *c = static_cast<WsClient *>(httpd_sess_get_ctx(s_https_server, fd));
        if (c != nullptr && c->events) {
            if (events_flush(fd, c, svs_key) != ESP_OK) {
                httpd_sess_trigger_close(s_https_server, fd);
            } else {
                httpd_sess_update_lru_counter(s_https_server, fd);
            }
            continue;
        }
        if (c == nullptr || !c->ready) {
            continue;
        }
        if (!auth::session_valid(c->token)) {
            ws_send_text(fd, "{\"type\":\"auth\"}");
            httpd_sess_trigger_close(s_https_server, fd);
            continue;
        }
        if (svs.empty()) {
            svs = ws_svs_message();
        }
        esp_err_t err = ESP_OK;
        if (svs != c->svs) {
            err = ws_send_text(fd, svs);
            c->svs = svs;
        }
        if (err == ESP_OK && svs_usb::log_head() > c->log_after) {
            JsonDocument doc;
            doc["type"] = "log";
            size_t count = add_log_entries(doc, c->log_after, &c->log_after);
            std::string out;
            serializeJson(doc, out);
            err = ws_send_text(fd, out);
            more |= count == LOG_BATCH;
        }
        if (err != ESP_OK) {
            httpd_sess_trigger_close(s_https_server, fd);
        } else {
            httpd_sess_update_lru_counter(s_https_server, fd);  // in use: not the one to purge
        }
    }
    if (more) {
        ws_notify();
    }
}

static esp_err_t ws_reply(httpd_req_t *req, const std::string &text)
{
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)text.data();
    frame.len = text.size();
    return httpd_ws_send_frame(req, &frame);
}

// A message from a page
static esp_err_t ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK || frame.len > MAX_WS_FRAME) {  // its length
        return ESP_FAIL;
    }
    std::string text(frame.len, '\0');
    if (frame.len > 0) {
        frame.payload = (uint8_t *)&text[0];
        if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;  // a pong
    }
    auto *c = static_cast<WsClient *>(req->sess_ctx);
    if (c == nullptr) {
        return ESP_FAIL;
    }
    if (!auth::session_valid(c->token)) {
        ws_reply(req, "{\"type\":\"auth\"}");
        return ESP_FAIL;
    }
    JsonDocument in;
    if (deserializeJson(in, text) != DeserializationError::Ok) {
        return ESP_OK;
    }
    std::string type = in["type"] | "";
    if (type == "hello") {
        c->log_after = in["after"].as<uint32_t>();
        c->svs.clear();
        c->ready = true;
        ws_notify();
        return ESP_OK;
    }
    if (type == "ping") {
        return ws_reply(req, "{\"type\":\"pong\"}");
    }
    if (type == "send") {
        JsonDocument out;
        out["type"] = "reply";
        out["id"] = in["id"];
        std::string error;
        int status = svs_send_command(in["command"] | "", error);
        if (status == 0) {
            out["ok"] = true;
        } else {
            out["status"] = status;
            out["error"] = error;
        }
        std::string reply;
        serializeJson(out, reply);
        return ws_reply(req, reply);  // the command's line reaches the log by itself (ws_notify)
    }
    return ESP_OK;
}

// A browser sends Origin with every WebSocket handshake: only this bridge's own
// page may open one with the session cookie (SameSite=Strict already keeps
// other sites' pages from sending it)
static bool same_origin(httpd_req_t *req)
{
    char origin[160];
    char host[128];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
    if (err == ESP_ERR_NOT_FOUND) {
        return true;  // not a browser: the cookie is all it has
    }
    if (err != ESP_OK || httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    return std::string(origin) == std::string("https://") + host;
}

// Before the handshake: an admin session, from the bridge's page
static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    if (!same_origin(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Not this bridge's page");
        return ESP_FAIL;
    }
    std::string token = session_token(req);
    if (token.empty() || !auth::session_valid(token)) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Login required");
        return ESP_FAIL;
    }
    auto *c = new (std::nothrow) WsClient();
    if (c == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    c->token = token;
    s_ws_clients++;
    req->sess_ctx = c;  // freed with the session
    req->free_ctx = ws_client_free;
    return ESP_OK;
}

// Room for one more /api/v1/events client: at EVENTS_MAX, the oldest goes (usually a Home Assistant
// that reconnected before its old connection was noticed gone)
static void events_make_room()
{
    int fds[HTTPS_MAX_SOCKETS];
    size_t n = HTTPS_MAX_SOCKETS;
    if (httpd_get_client_list(s_https_server, &n, fds) != ESP_OK) {
        return;
    }
    int count = 0;
    int oldest = -1;
    int64_t oldest_us = 0;
    for (size_t i = 0; i < n; i++) {
        if (httpd_ws_get_fd_info(s_https_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
            continue;
        }
        auto *c = static_cast<WsClient *>(httpd_sess_get_ctx(s_https_server, fds[i]));
        if (c == nullptr || !c->events) {
            continue;
        }
        count++;
        if (oldest < 0 || c->opened_us < oldest_us) {
            oldest = fds[i];
            oldest_us = c->opened_us;
        }
    }
    if (count >= EVENTS_MAX && oldest >= 0) {
        ESP_LOGI(TAG, "events: %d sockets open, closing the oldest", count);
        httpd_sess_trigger_close(s_https_server, oldest);
    }
}

// Before the /api/v1/events handshake: the API token, and the event types asked for (?types=)
static esp_err_t events_pre_handshake(httpd_req_t *req)
{
    std::string token = bearer_token(req);
    if (token.empty() || !auth::api_token_valid(token)) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Invalid or missing API token");
        return ESP_FAIL;
    }
    uint32_t types = api_events::STATE;
    char query[160];
    char list[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "types", list, sizeof(list)) == ESP_OK) {
        types = api_events::parse(api_events::url_decode(list));
    }
    auto *c = new (std::nothrow) WsClient();
    if (c == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    events_make_room();
    c->events = true;
    c->token = token;
    c->types = types;
    c->opened_us = esp_timer_get_time();
    s_ws_clients++;
    req->sess_ctx = c;  // freed with the session
    req->free_ctx = ws_client_free;
    return ESP_OK;  // its "hello" goes with the next flush (within WS_TICK_MS)
}

// A message from an events client: reserved (none is defined yet), so read and ignored
static esp_err_t events_handler(httpd_req_t *req)
{
    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK || frame.len > MAX_WS_FRAME) {  // its length
        return ESP_FAIL;
    }
    if (frame.len > 0) {
        std::string text(frame.len, '\0');
        frame.payload = (uint8_t *)&text[0];
        if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Dispatch and fallbacks
// ---------------------------------------------------------------------------

static esp_err_t dispatch(httpd_req_t *req)
{
    const auto *route = (const Route *)req->user_ctx;
    if (route->portal_only && !wifi_manager::ap_active()) {
        return redirect_to_https(req);
    }
    if (route->access == Access::Admin && !authenticated(req)) {
        JsonDocument doc;
        doc["error"] = "Login required";
        return send_status_json(req, "401 Unauthorized", doc);
    }
    if (route->access == Access::Api && !api_authorized(req)) {
        JsonDocument doc;
        doc["error"] = "Invalid or missing API token";
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Bearer");
        return send_status_json(req, "401 Unauthorized", doc);
    }
    return route->handler(req);
}

static esp_err_t https_not_found(httpd_req_t *req, httpd_err_code_t err)
{
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
}

// Any other plain HTTP URL: captive portal redirect, or HTTPS when not in setup mode
static esp_err_t http_not_found(httpd_req_t *req, httpd_err_code_t err)
{
    if (wifi_manager::ap_active()) {
        return redirect(req, "302 Found", PORTAL_URL);
    }
    return redirect_to_https(req);
}

// ---------------------------------------------------------------------------

struct RouteEntry {
    const char *uri;
    httpd_method_t method;
    Route route;
};

// HTTPS: the web UI. Everything that manages the bridge needs a login.
static const RouteEntry HTTPS_ROUTES[] = {
    {"/", HTTP_GET, {index_get, Access::Public, false}},
    {"/api/status", HTTP_GET, {status_get, Access::Public, false}},
    {"/api/v1/info", HTTP_GET, {api_info_get, Access::Api, false}},
    {"/api/v1/state", HTTP_GET, {api_state_get, Access::Api, false}},
    {"/device/api-token", HTTP_GET, {api_token_get, Access::Admin, false}},
    {"/device/api-token/regenerate", HTTP_POST, {api_token_regenerate_post, Access::Admin, false}},
    {"/device/auth", HTTP_GET, {auth_get, Access::Public, false}},
    {"/device/setup-password", HTTP_POST, {setup_password_post, Access::Public, false}},
    {"/device/login", HTTP_POST, {login_post, Access::Public, false}},
    {"/device/logout", HTTP_POST, {logout_post, Access::Public, false}},
    {"/device/scan", HTTP_GET, {scan_get, Access::Admin, false}},
    {"/device/clients", HTTP_GET, {clients_get, Access::Admin, false}},
    {"/device/cert", HTTP_GET, {cert_get, Access::Admin, false}},
    {"/device/wifi", HTTP_POST, {wifi_post, Access::Admin, false}},
    {"/device/ota", HTTP_POST, {ota_post, Access::Admin, false}},
    {"/device/releases", HTTP_GET, {releases_get, Access::Admin, false}},
    {"/device/ota/github", HTTP_POST, {ota_github_post, Access::Admin, false}},
    {"/device/reboot", HTTP_POST, {reboot_post, Access::Admin, false}},
    {"/device/factory-reset", HTTP_POST, {factory_reset_post, Access::Admin, false}},
    {"/device/svs", HTTP_GET, {svs_get, Access::Admin, false}},
    {"/device/svs/log", HTTP_GET, {svs_log_get, Access::Admin, false}},
    {"/device/svs/restart", HTTP_POST, {svs_restart_post, Access::Admin, false}},
    {"/device/svs/send", HTTP_POST, {svs_send_post, Access::Admin, false}},
    {"/device/svs/check", HTTP_POST, {svs_check_post, Access::Admin, false}},
    {"/device/svs/preview", HTTP_POST, {svs_preview_post, Access::Admin, false}},
    {"/device/svs/probe", HTTP_POST, {svs_probe_post, Access::Admin, false}},
    {"/device/svs/releases", HTTP_GET, {svs_releases_get, Access::Admin, false}},
    {"/device/svs/firmware", HTTP_POST, {svs_firmware_upload_post, Access::Admin, false}},
    {"/device/svs/firmware/official", HTTP_POST, {svs_firmware_official_post, Access::Admin, false}},
    {"/device/svs/firmware/flash", HTTP_POST, {svs_flash_post, Access::Admin, false}},
    {"/device/svs/config", HTTP_GET, {svs_config_get, Access::Admin, false}},
    {"/device/svs/config/read", HTTP_POST, {svs_config_read_post, Access::Admin, false}},
    {"/device/svs/config/write", HTTP_POST, {svs_config_write_post, Access::Admin, false}},
    {"/device/svs/layout", HTTP_GET, {svs_layout_get, Access::Admin, false}},
    {"/device/svs/layout", HTTP_POST, {svs_layout_post, Access::Admin, false}},
    {"/device/cruller", HTTP_GET, {cruller_get, Access::Admin, false}},
    {"/device/cruller/scan", HTTP_POST, {cruller_scan_post, Access::Admin, false}},
    {"/device/cruller/select", HTTP_POST, {cruller_select_post, Access::Admin, false}},
};

// Plain HTTP: the setup portal (admin password, then WiFi), only while the
// setup access point is up.
static const RouteEntry HTTP_ROUTES[] = {
    {"/", HTTP_GET, {portal_get, Access::Public, true}},
    {"/api/status", HTTP_GET, {status_get, Access::Public, true}},
    {"/device/auth", HTTP_GET, {auth_get, Access::Public, true}},
    {"/device/setup-password", HTTP_POST, {setup_password_post, Access::Public, true}},
    {"/device/login", HTTP_POST, {login_post, Access::Public, true}},
    {"/device/scan", HTTP_GET, {scan_get, Access::Admin, true}},
    {"/device/wifi", HTTP_POST, {wifi_post, Access::Admin, true}},
};

template <size_t N>
static void register_routes(httpd_handle_t server, const RouteEntry (&routes)[N])
{
    for (const auto &entry : routes) {
        httpd_uri_t uri = {};
        uri.uri = entry.uri;
        uri.method = entry.method;
        uri.handler = dispatch;
        uri.user_ctx = (void *)&entry.route;
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));
    }
}

static void register_ws(httpd_handle_t server)
{
    httpd_uri_t uri = {};
    uri.uri = "/ws";
    uri.method = HTTP_GET;
    uri.handler = ws_handler;
    uri.is_websocket = true;  // the server answers pings and closes by itself
    uri.ws_pre_handshake_cb = ws_pre_handshake;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uri));

    httpd_uri_t events = {};
    events.uri = "/api/v1/events";
    events.method = HTTP_GET;
    events.handler = events_handler;
    events.is_websocket = true;
    events.ws_pre_handshake_cb = events_pre_handshake;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &events));
}

static void start_https()
{
    const std::string &cert = tls_cert::cert_pem();
    const std::string &key = tls_cert::key_pem();

    // IDF 6.1's HTTPD_SSL_CONFIG_DEFAULT() leaves use_secure_element out,
    // which C++ reports as an error (the field is zero-initialized anyway)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
    httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
#pragma GCC diagnostic pop
    // PEM lengths must include the terminating '\0'
    config.servercert = (const uint8_t *)cert.c_str();
    config.servercert_len = cert.size() + 1;
    config.prvtkey_pem = (const uint8_t *)key.c_str();
    config.prvtkey_len = key.size() + 1;
    config.httpd.max_uri_handlers = sizeof(HTTPS_ROUTES) / sizeof(HTTPS_ROUTES[0]) + 2;  // + /ws, /api/v1/events
    // Handlers that fetch SVS releases open their own TLS connection to GitHub
    config.httpd.stack_size = 16384;
    config.httpd.max_open_sockets = HTTPS_MAX_SOCKETS;
    // A page keeps its WebSocket open: TCP keepalive frees the slot of one whose
    // computer went away without closing it (asleep, off the network)
    config.httpd.keep_alive_enable = true;
    config.httpd.keep_alive_idle = 10;
    config.httpd.keep_alive_interval = 5;
    config.httpd.keep_alive_count = 3;

    ESP_ERROR_CHECK(httpd_ssl_start(&s_https_server, &config));
    register_routes(s_https_server, HTTPS_ROUTES);
    register_ws(s_https_server);
    ESP_ERROR_CHECK(httpd_register_err_handler(s_https_server, HTTPD_404_NOT_FOUND, https_not_found));
}

static void start_http()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 3;  // only the setup portal lives here
    config.max_uri_handlers = sizeof(HTTP_ROUTES) / sizeof(HTTP_ROUTES[0]);
    config.lru_purge_enable = true;

    httpd_handle_t server = nullptr;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    register_routes(server, HTTP_ROUTES);
    ESP_ERROR_CHECK(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_not_found));
}

void start()
{
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = on_reboot_timer;
    timer_args.name = "reboot";
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reboot_timer));

    start_https();
    start_http();

    // Pages on /ws hear of each new log line at once, and of the rest within a tick
    svs_usb::set_log_listener(ws_notify);
    esp_timer_create_args_t tick_args = {};
    tick_args.callback = on_ws_tick;
    tick_args.name = "ws";
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &s_ws_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_ws_timer, WS_TICK_MS * 1000));
    ESP_LOGI(TAG, "Web servers started (HTTPS 443, HTTP 80)");
}

}  // namespace web_server
