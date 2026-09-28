#include "cruller.h"

#include <string.h>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mdns.h"
#include "nvs.h"
#include "ArduinoJson.h"

#include "svs_settings.h"
#include "svs_usb.h"
#include "wifi_manager.h"

namespace cruller {

static const char *TAG = "cruller";

static const char *NVS_NAMESPACE = "cruller";
static const char *KEY_ID = "id";
static const size_t MAX_ID_LEN = 64;

static const int POLL_MS = 250;                 // how often the active input is checked
static const int64_t HEARTBEAT_MS = 60 * 1000;  // repeat the report, in case one was lost
static const int64_t RETRY_MS = 10 * 1000;      // after a report that did not get through
static const int64_t SCAN_MS = 30 * 1000;       // browse the network again
static const int64_t FORGET_MS = 100 * 1000;    // drop a Cruller not heard of for this long
static const int64_t ANNOUNCE_GAP_MS = 3000;    // announcements report at most this often
static const int64_t DESCRIBE_MS = 1000;        // how often the switch's description is checked
static const uint32_t QUERY_MS = 2000;
static const size_t MAX_RESULTS = 16;
static const int HTTP_TIMEOUT_MS = 4000;

static SemaphoreHandle_t s_mutex = nullptr;       // guards everything below
static SemaphoreHandle_t s_scan_mutex = nullptr;  // one mDNS query at a time
static TaskHandle_t s_task = nullptr;
static std::string s_selected;
static std::vector<Found> s_found;
static Report s_last = {false, false, 0, "", "", "", -1, 0, 0};
static bool s_announced = false;  // the selected Cruller (re)announced itself

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

static void wake()
{
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

static std::string txt_value(const mdns_result_t *r, const char *key)
{
    for (size_t i = 0; i < r->txt_count; i++) {
        if (r->txt[i].key != nullptr && strcmp(r->txt[i].key, key) == 0) {
            if (r->txt[i].value == nullptr) {
                return "";
            }
            size_t len = r->txt_value_len ? r->txt_value_len[i] : strlen(r->txt[i].value);
            return std::string(r->txt[i].value, len);
        }
    }
    return "";
}

// A Cruller from an mDNS answer; false if it carries no id
static bool to_found(const mdns_result_t *r, Found &f)
{
    f.id = txt_value(r, "id");
    if (f.id.empty() || f.id.size() > MAX_ID_LEN) {
        return false;
    }
    f.name = txt_value(r, "name");
    f.version = txt_value(r, "ver");
    f.instance = r->instance_name ? r->instance_name : "";
    f.host = r->hostname ? r->hostname : "";
    f.port = r->port ? r->port : 80;
    f.ip.clear();
    for (const mdns_ip_addr_t *a = r->addr; a != nullptr; a = a->next) {
        if (a->addr.type == ESP_IPADDR_TYPE_V4) {
            char buf[16];
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&a->addr.u_addr.ip4));
            f.ip = buf;
            break;
        }
    }
    f.seen_ms = now_ms();
    return true;
}

// Adds or refreshes a Cruller in s_found (s_mutex held). An answer without an
// address or host keeps the ones already known.
static void upsert(const Found &f)
{
    for (auto &e : s_found) {
        if (e.id == f.id) {
            std::string ip = f.ip.empty() ? e.ip : f.ip;
            std::string host = f.host.empty() ? e.host : f.host;
            e = f;
            e.ip = ip;
            e.host = host;
            return;
        }
    }
    s_found.push_back(f);
}

#ifdef CONFIG_MDNS_ENABLE_BROWSE
// Runs in the mDNS task, holding its lock: copy what is needed, never call
// mDNS from here. s_mutex is never held while calling mDNS, so taking it here
// cannot deadlock.
static void on_browse(mdns_result_t *result)
{
    Found f;
    if (result == nullptr || !to_found(result, f)) {
        return;
    }
    bool selected;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (result->ttl == 0) {  // goodbye: it is shutting down
        s_found.erase(std::remove_if(s_found.begin(), s_found.end(),
                                     [&](const Found &e) { return e.id == f.id; }),
                      s_found.end());
        selected = false;
    } else {
        upsert(f);
        selected = f.id == s_selected;
        if (selected) {
            s_announced = true;
        }
    }
    xSemaphoreGive(s_mutex);
    if (selected) {
        wake();
    }
}
#endif

void scan()
{
    if (!wifi_manager::sta_connected()) {
        return;
    }
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    mdns_result_t *results = nullptr;
    esp_err_t err = mdns_query_ptr("_rt4k", "_tcp", QUERY_MS, MAX_RESULTS, &results);
    xSemaphoreGive(s_scan_mutex);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS query failed: %s", esp_err_to_name(err));
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (const mdns_result_t *r = results; r != nullptr; r = r->next) {
        Found f;
        if (to_found(r, f)) {
            upsert(f);
        }
    }
    int64_t now = now_ms();
    s_found.erase(std::remove_if(s_found.begin(), s_found.end(),
                                 [&](const Found &e) { return now - e.seen_ms > FORGET_MS; }),
                  s_found.end());
    xSemaphoreGive(s_mutex);
    mdns_query_results_free(results);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

// The switch as the SVS tab sets it up, for Cruller: the SVS's firmware, each input and output with
// its module and name (the layout), and the input settings read from the SVS, once they have been
// read. An object: {"firmware", "inputs": [...], "outputs": [...]}.
static std::string describe_switch(const svs_usb::Info &info)
{
    JsonDocument layout;
    if (deserializeJson(layout, svs_settings::layout_json()) != DeserializationError::Ok) {
        layout.clear();
    }
    const svs_settings::Snapshot sn = svs_settings::status().snapshot;
    const int read = sn.valid ? sn.inputs : 0;

    JsonDocument doc;
    doc["firmware"] = info.firmware;
    JsonArrayConst names = layout["inputs"].as<JsonArrayConst>();
    JsonArray inputs = doc["inputs"].to<JsonArray>();
    const int n = std::max((int)names.size(), read);
    for (int i = 0; i < n; i++) {
        JsonObjectConst l = names[i].as<JsonObjectConst>();
        JsonObject o = inputs.add<JsonObject>();
        o["kind"] = l["kind"] | "";
        o["name"] = l["name"] | "";
        if (i < read) {  // only what was read from the SVS: absent means not known
            const svs_config::InputSettings &in = sn.settings[i];
            o["auto_profile"] = in.auto_profile;
            o["rgsb"] = in.rgsb;
            o["sync_bypass"] = in.sync_bypass;
            o["rgb_to_ypbpr"] = in.rgb_to_ypbpr;
            o["ypbpr_to_rgb"] = in.ypbpr_to_rgb;
            o["v3"] = (bool)sn.hardware.scart_v3[i] || (bool)sn.hardware.vga_v3[i];
        }
    }
    JsonArray outputs = doc["outputs"].to<JsonArray>();
    for (JsonObjectConst l : layout["outputs"].as<JsonArrayConst>()) {
        JsonObject o = outputs.add<JsonObject>();
        o["kind"] = l["kind"] | "";
        o["name"] = l["name"] | "";
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

// Sends the active input and the switch's description (describe_switch()) to Cruller c and fills r
// with the outcome
static void report(const Found &c, const svs_usb::Info &info, const std::string &described, Report &r)
{
    r.attempted = true;
    r.ok = false;
    r.http_status = 0;
    r.error.clear();
    r.paired_with.clear();
    r.to_id = c.id;
    r.input = info.current_input;
    r.at_ms = now_ms();

    std::string host = !c.ip.empty() ? c.ip : !c.host.empty() ? c.host + ".local" : "";
    if (host.empty()) {
        r.error = "No address for this Cruller yet";
        return;
    }
    std::string url = "http://" + host + ":" + std::to_string(c.port) + "/api/svs";

    // The same names as the "svs" object of /api/v1/state, plus the bridge's id, then the
    // switch's description
    JsonDocument doc;
    doc["id"] = wifi_manager::device_id();
    doc["current_input"] = info.current_input;
    if (info.total_inputs >= 0) {
        doc["total_inputs"] = info.total_inputs;
    }
    doc["live"] = info.inputs_live;
    std::string body;
    serializeJson(doc, body);
    if (described.size() > 2) {  // {"a":1} + {"b":2} -> {"a":1,"b":2}
        body.pop_back();
        body += "," + described.substr(1);
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.user_agent = "svs-bridge";
    config.disable_auto_redirect = true;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        r.error = "Out of memory";
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    char resp[256] = {};
    if (esp_http_client_open(client, body.size()) != ESP_OK) {
        r.error = "Could not reach Cruller at " + host;
    } else if (esp_http_client_write(client, body.data(), body.size()) != (int)body.size()) {
        r.error = "Connection lost while sending";
    } else if (esp_http_client_fetch_headers(client) < 0) {
        r.error = "No answer from Cruller";
    } else {
        r.http_status = esp_http_client_get_status_code(client);
        esp_http_client_read_response(client, resp, sizeof(resp) - 1);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (r.http_status == 0) {
        return;
    }

    JsonDocument answer;
    bool parsed = deserializeJson(answer, resp) == DeserializationError::Ok;
    if (r.http_status == 200 && parsed && (answer["ok"] | false)) {
        r.ok = true;
    } else if (r.http_status == 409) {
        r.error = parsed ? (answer["error"] | "paired with another SVS Bridge")
                         : "paired with another SVS Bridge";
        r.paired_with = parsed ? (answer["paired"] | "") : "";
    } else if (r.http_status == 404) {
        r.error = "This Cruller does not take SVS reports yet (update it)";
    } else {
        r.error = "HTTP " + std::to_string(r.http_status);
        if (parsed && answer["error"].is<const char *>()) {
            r.error += ": " + std::string(answer["error"].as<const char *>());
        }
    }
}

static void task(void *arg)
{
    int64_t next_scan = 0;
    int64_t next_report = 0;
    int64_t last_report = 0;
    std::string sent_to;  // the Cruller last reported to; empty = report on finding it
    int sent_input = -1;
    int sent_total = -1;
    std::string described;       // the switch's description, as last checked
    std::string sent_described;  // ... and as last sent
    int64_t next_describe = 0;
    bool was_connected = false;
    std::string last_error;  // logged once, not on every retry

    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));

        if (!wifi_manager::sta_connected()) {
            was_connected = false;
            continue;
        }
        if (!was_connected) {  // (re)joined the network: look again and report again
            was_connected = true;
            next_scan = 0;
            sent_to.clear();
        }
        if (now_ms() >= next_scan) {
            scan();
            next_scan = now_ms() + SCAN_MS;
        }

        Found target;
        bool found = false;
        bool announced;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        for (const auto &e : s_found) {
            if (e.id == s_selected) {
                target = e;
                found = true;
                break;
            }
        }
        announced = s_announced;
        s_announced = false;
        xSemaphoreGive(s_mutex);

        if (!found) {
            sent_to.clear();
            continue;
        }
        svs_usb::Info info = svs_usb::info();
        if (info.current_input < 0) {
            continue;  // nothing to say until the SVS reports its input
        }

        int64_t now = now_ms();
        if (now >= next_describe) {  // the layout edited, the SVS's settings read or saved
            described = describe_switch(info);
            next_describe = now + DESCRIBE_MS;
        }
        bool due = sent_to != target.id || info.current_input != sent_input ||
                   info.total_inputs != sent_total || described != sent_described || now >= next_report ||
                   (announced && now - last_report >= ANNOUNCE_GAP_MS);
        if (!due) {
            continue;
        }

        Report r;
        report(target, info, described, r);
        bool changed = sent_to != target.id || info.current_input != sent_input;
        sent_to = target.id;
        sent_input = info.current_input;
        sent_total = info.total_inputs;
        sent_described = described;
        last_report = now_ms();
        // Unreachable: retry sooner, and look it up again in case its address changed
        next_report = last_report + (r.http_status != 0 ? HEARTBEAT_MS : RETRY_MS);
        if (r.http_status == 0) {
            next_scan = std::min(next_scan, last_report + RETRY_MS);
        }

        if (r.ok && (changed || !last_error.empty())) {
            ESP_LOGI(TAG, "Input %d reported to %s", r.input, target.id.c_str());
        } else if (!r.ok && r.error != last_error) {
            ESP_LOGW(TAG, "Report to %s failed: %s", target.id.c_str(), r.error.c_str());
        }
        last_error = r.ok ? "" : r.error;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        r.count = s_last.count + (r.ok ? 1 : 0);
        if (s_selected == target.id) {  // not changed meanwhile
            s_last = r;
        }
        xSemaphoreGive(s_mutex);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

State state()
{
    State st;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    st.selected_id = s_selected;
    st.found = s_found;
    st.last = s_last;
    xSemaphoreGive(s_mutex);
    auto label = [](const Found &f) { return f.name.empty() ? f.instance : f.name; };
    std::sort(st.found.begin(), st.found.end(),
              [&](const Found &a, const Found &b) { return label(a) < label(b); });
    return st;
}

esp_err_t select(const std::string &id)
{
    if (id.size() > MAX_ID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = id.empty() ? nvs_erase_key(h, KEY_ID) : nvs_set_str(h, KEY_ID, id.c_str());
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;  // nothing to forget
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_selected != id) {
        s_selected = id;
        s_last = {false, false, 0, "", "", "", -1, 0, 0};
    }
    xSemaphoreGive(s_mutex);
    if (id.empty()) {
        ESP_LOGI(TAG, "Not reporting to any Cruller");
    } else {
        ESP_LOGI(TAG, "Reporting to Cruller %s", id.c_str());
    }
    wake();
    return ESP_OK;
}

void start()
{
    s_mutex = xSemaphoreCreateMutex();
    s_scan_mutex = xSemaphoreCreateMutex();

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        char buf[MAX_ID_LEN + 1];
        size_t len = sizeof(buf);
        if (nvs_get_str(h, KEY_ID, buf, &len) == ESP_OK) {
            s_selected = buf;
        }
        nvs_close(h);
    }
    if (s_selected.empty()) {
        ESP_LOGI(TAG, "No Cruller chosen");
    } else {
        ESP_LOGI(TAG, "Reporting to Cruller %s", s_selected.c_str());
    }

    xTaskCreate(task, "cruller", 6144, NULL, 4, &s_task);

#ifdef CONFIG_MDNS_ENABLE_BROWSE
    // Announcements (a Cruller starting up) arrive here without waiting for
    // the next scan
    if (mdns_browse_new("_rt4k", "_tcp", on_browse) == nullptr) {
        ESP_LOGW(TAG, "mDNS browse failed; relying on periodic scans");
    }
#endif
}

}  // namespace cruller
