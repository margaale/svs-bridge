#include "web_server.h"

#include <stdlib.h>
#include <string.h>
#include <string>
#include <memory>
#include <functional>

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

#include "auth.h"
#include "bridge_fw_repo.h"
#include "factory_reset.h"
#include "svs_flasher.h"
#include "svs_fw_repo.h"
#include "svs_usb.h"
#include "tls_cert.h"
#include "wifi_manager.h"

namespace web_server {

static const char *TAG = "web";

static const size_t MAX_JSON_BODY = 512;
static const size_t MAX_SVS_HEX = 256 * 1024;  // official SVS .hex files are ~85 KB
static const size_t OTA_CHUNK = 16384;  // one full TLS record per read
static const char *PORTAL_URL = "http://192.168.4.1/";

extern "C" const char index_html_start[] asm("_binary_index_html_start");
extern "C" const char index_html_end[] asm("_binary_index_html_end");
extern "C" const char portal_html_start[] asm("_binary_portal_html_start");
extern "C" const char portal_html_end[] asm("_binary_portal_html_end");

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

// The bridge is set to only listen to the SVS, not talk back
static esp_err_t send_listen_only(httpd_req_t *req)
{
    JsonDocument doc;
    doc["error"] = "The bridge is in listen-only mode. Turn on \"Send commands\" first "
                   "(disconnect the RetroTINK's HD-15 before sending or flashing).";
    httpd_resp_set_status(req, "403 Forbidden");
    return send_json(req, doc);
}

// Rebooting or updating the bridge now would interrupt the SVS update
static esp_err_t send_busy(httpd_req_t *req)
{
    JsonDocument doc;
    doc["error"] = "An SVS firmware update is in progress";
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

static esp_err_t send_html(httpd_req_t *req, const char *start, const char *end)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, start, end - start);
}

static esp_err_t index_get(httpd_req_t *req)
{
    return send_html(req, index_html_start, index_html_end);
}

static esp_err_t portal_get(httpd_req_t *req)
{
    return send_html(req, portal_html_start, portal_html_end);
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
    svs["send_enabled"] = svs_usb::send_allowed();
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

// ---------------------------------------------------------------------------
// Public API (/api/v1) for external integrations, e.g. Home Assistant.
// Bearer-authenticated with the API token; independent of the admin session.
// ---------------------------------------------------------------------------

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
    doc["api_version"] = 1;
    return send_json(req, doc);
}

// Live state, polled by the integration.
static esp_err_t api_state_get(httpd_req_t *req)
{
    JsonDocument doc;
    add_svs_info(doc["svs"].to<JsonObject>());
    JsonObject bridge = doc["bridge"].to<JsonObject>();
    bridge["rssi"] = wifi_manager::sta_rssi();
    bridge["uptime_s"] = esp_timer_get_time() / 1000000;
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
                           std::string &err_msg)
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
    if (svs_flasher::busy()) {
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

// Opens `url`, following up to 5 redirects (GitHub asset URLs redirect to a
// CDN). On success returns the ready-to-read client via *out; caller closes it.
static esp_err_t open_following_redirects(const std::string &url, esp_http_client_handle_t *out,
                                          std::string &err)
{
    std::string cur = url;
    for (int hop = 0; hop < 5; hop++) {
        esp_http_client_config_t config = {};
        config.url = cur.c_str();
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.timeout_ms = 20000;
        config.buffer_size = 4096;
        config.user_agent = "svs-bridge";
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            err = "Out of memory";
            return ESP_ERR_NO_MEM;
        }
        if (esp_http_client_open(client, 0) != ESP_OK) {
            esp_http_client_cleanup(client);
            err = "Could not reach GitHub";
            return ESP_FAIL;
        }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char *loc = nullptr;
            if (esp_http_client_get_header(client, "Location", &loc) == ESP_OK && loc != nullptr) {
                cur = loc;  // copied before cleanup frees the header
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                continue;
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            err = "Redirect without a location";
            return ESP_FAIL;
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
    err = "Too many redirects";
    return ESP_FAIL;
}

// Downloads a chosen GitHub release's firmware and applies it over the air.
static esp_err_t ota_github_post(httpd_req_t *req)
{
    if (svs_flasher::busy()) {
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
    if (open_following_redirects(url, &client, err) != ESP_OK) {
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, err.c_str());
    }
    auto read = [&](uint8_t *b, size_t want) -> int {
        return esp_http_client_read(client, (char *)b, want);
    };
    esp_err_t rc = ota_apply(size, read, err);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (rc != ESP_OK) {
        return send_error(req, HTTPD_400_BAD_REQUEST, err.c_str());
    }
    send_ok(req);
    schedule_reboot();
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (svs_flasher::busy()) {
        return send_busy(req);
    }
    send_ok(req);
    schedule_reboot();
    return ESP_OK;
}

// Erases all settings (WiFi, TLS certificate, ...) and reboots into setup mode
static esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (svs_flasher::busy()) {
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
static esp_err_t svs_get(httpd_req_t *req)
{
    JsonDocument doc;
    add_svs_info(doc.to<JsonObject>());

    svs_flasher::Status st = svs_flasher::status();
    JsonObject update = doc["update"].to<JsonObject>();
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
    return send_json(req, doc);
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
    doc["now"] = esp_timer_get_time() / 1000;
    JsonArray entries = doc["entries"].to<JsonArray>();
    for (const auto &e : svs_usb::log_since(after, 100)) {
        JsonObject o = entries.add<JsonObject>();
        o["seq"] = e.seq;
        o["t"] = e.ms;
        o["d"] = std::string(1, e.dir);
        o["s"] = e.text;
    }
    return send_json(req, doc);
}

// Body: {"command": "SVS_Input_Up"}; sent as typed, plus the configured line ending
static esp_err_t svs_send_post(httpd_req_t *req)
{
    if (!svs_usb::send_allowed()) {
        return send_listen_only(req);
    }
    JsonDocument body;
    if (!read_json_body(req, body)) {
        return ESP_OK;
    }
    std::string command = body["command"] | "";
    if (command.empty() || command.size() > 128) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "Enter a command (up to 128 characters)");
    }
    esp_err_t err = svs_usb::send(command);
    if (err == ESP_ERR_NOT_ALLOWED) {
        return send_busy(req);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "The SVS is not connected");
    }
    if (err != ESP_OK) {
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    return send_ok(req);
}

// Sets whether the bridge may send to the SVS. Body: {"send": bool}
static esp_err_t svs_mode_post(httpd_req_t *req)
{
    if (svs_flasher::busy()) {
        return send_busy(req);
    }
    JsonDocument body;
    if (!read_json_body(req, body)) {
        return ESP_OK;
    }
    svs_usb::set_send_allowed(body["send"] | false);
    return svs_get(req);
}

// Restarts the SVS so it reports its firmware version and inputs again
static esp_err_t svs_restart_post(httpd_req_t *req)
{
    if (!svs_usb::send_allowed()) {
        return send_listen_only(req);
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
    if (!svs_usb::send_allowed()) {
        return send_listen_only(req);
    }
    if (svs_flasher::busy()) {
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
    if (!svs_usb::send_allowed()) {
        return send_listen_only(req);
    }
    if (svs_flasher::busy()) {
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
    if (!svs_usb::send_allowed()) {
        return send_listen_only(req);
    }
    if (svs_flasher::busy()) {
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
    if (svs_flasher::busy()) {
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
    if (svs_flasher::busy()) {
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
    if (!svs_usb::send_allowed()) {
        return send_listen_only(req);
    }
    if (svs_flasher::busy()) {
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
static bool api_authorized(httpd_req_t *req)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 0 || len > 128) {
        return false;
    }
    std::string header(len, '\0');
    if (httpd_req_get_hdr_value_str(req, "Authorization", &header[0], len + 1) != ESP_OK) {
        return false;
    }
    const std::string prefix = "Bearer ";
    if (header.rfind(prefix, 0) != 0) {
        return false;
    }
    return auth::api_token_valid(header.substr(prefix.size()));
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
    {"/device/cert", HTTP_GET, {cert_get, Access::Admin, false}},
    {"/device/wifi", HTTP_POST, {wifi_post, Access::Admin, false}},
    {"/device/ota", HTTP_POST, {ota_post, Access::Admin, false}},
    {"/device/releases", HTTP_GET, {releases_get, Access::Admin, false}},
    {"/device/ota/github", HTTP_POST, {ota_github_post, Access::Admin, false}},
    {"/device/reboot", HTTP_POST, {reboot_post, Access::Admin, false}},
    {"/device/factory-reset", HTTP_POST, {factory_reset_post, Access::Admin, false}},
    {"/device/svs", HTTP_GET, {svs_get, Access::Admin, false}},
    {"/device/svs/log", HTTP_GET, {svs_log_get, Access::Admin, false}},
    {"/device/svs/mode", HTTP_POST, {svs_mode_post, Access::Admin, false}},
    {"/device/svs/restart", HTTP_POST, {svs_restart_post, Access::Admin, false}},
    {"/device/svs/send", HTTP_POST, {svs_send_post, Access::Admin, false}},
    {"/device/svs/check", HTTP_POST, {svs_check_post, Access::Admin, false}},
    {"/device/svs/preview", HTTP_POST, {svs_preview_post, Access::Admin, false}},
    {"/device/svs/probe", HTTP_POST, {svs_probe_post, Access::Admin, false}},
    {"/device/svs/releases", HTTP_GET, {svs_releases_get, Access::Admin, false}},
    {"/device/svs/firmware", HTTP_POST, {svs_firmware_upload_post, Access::Admin, false}},
    {"/device/svs/firmware/official", HTTP_POST, {svs_firmware_official_post, Access::Admin, false}},
    {"/device/svs/firmware/flash", HTTP_POST, {svs_flash_post, Access::Admin, false}},
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
    config.httpd.max_uri_handlers = sizeof(HTTPS_ROUTES) / sizeof(HTTPS_ROUTES[0]);
    // Handlers that fetch SVS releases open their own TLS connection to GitHub
    config.httpd.stack_size = 16384;

    ESP_ERROR_CHECK(httpd_ssl_start(&s_https_server, &config));
    register_routes(s_https_server, HTTPS_ROUTES);
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
    ESP_LOGI(TAG, "Web servers started (HTTPS 443, HTTP 80)");
}

}  // namespace web_server
