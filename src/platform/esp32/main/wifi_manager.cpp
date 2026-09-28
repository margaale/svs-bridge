#include "wifi_manager.h"

#include <string.h>
#include <algorithm>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mdns.h"
#include "ArduinoJson.h"

#include "dns_server.h"

namespace wifi_manager {

static const char *TAG = "wifi";

// How long the station may try before the setup access point comes up
static const int64_t FALLBACK_AP_DELAY_US = 15 * 1000 * 1000;
// How long the access point stays up after the station connects: just enough
// for the portal page to show the result and the new address. Closing it
// then disconnects the phone, which goes back to its usual network.
static const int64_t AP_SHUTDOWN_DELAY_US = 10 * 1000 * 1000;
// Reconnect delays. Connection attempts make the radio leave the access
// point's channel, so retry less often while someone may be using the portal.
static const int64_t RETRY_DELAY_US = 5 * 1000 * 1000;
static const int64_t RETRY_DELAY_AP_US = 30 * 1000 * 1000;
static const int MAX_SCAN_RESULTS = 20;

static esp_netif_t *s_sta_netif = nullptr;
static esp_netif_t *s_ap_netif = nullptr;
static esp_timer_handle_t s_fallback_timer = nullptr;
static esp_timer_handle_t s_retry_timer = nullptr;
static esp_timer_handle_t s_ap_stop_timer = nullptr;
static SemaphoreHandle_t s_scan_mutex = nullptr;
static std::string s_ap_ssid;
static volatile bool s_has_credentials = false;
static volatile bool s_sta_connected = false;
static volatile bool s_ap_active = false;

static void restart_timer(esp_timer_handle_t timer, int64_t timeout_us)
{
    esp_timer_stop(timer);  // fails harmlessly if it was not running
    esp_timer_start_once(timer, timeout_us);
}

// ---------------------------------------------------------------------------
// Access point
// ---------------------------------------------------------------------------

static void start_ap()
{
    if (s_ap_active) {
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    wifi_config_t ap_config = {};
    strlcpy((char *)ap_config.ap.ssid, s_ap_ssid.c_str(), sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = s_ap_ssid.size();
    ap_config.ap.channel = 1;  // follows the station's channel once it connects
    ap_config.ap.max_connection = 4;
    if (strlen(CONFIG_SVS_AP_PASSWORD) >= 8) {
        strlcpy((char *)ap_config.ap.password, CONFIG_SVS_AP_PASSWORD, sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    esp_netif_ip_info_t ip_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_ap_netif, &ip_info));
    dns_server::start(ip_info.ip.addr);

    s_ap_active = true;
    ESP_LOGI(TAG, "Setup access point \"%s\" up, portal at http://" IPSTR "/",
             s_ap_ssid.c_str(), IP2STR(&ip_info.ip));
}

static void stop_ap()
{
    if (!s_ap_active) {
        return;
    }
    dns_server::stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_ap_active = false;
    ESP_LOGI(TAG, "Setup access point down");
}

// ---------------------------------------------------------------------------
// Timers and events
// ---------------------------------------------------------------------------

static void on_fallback_timer(void *arg)
{
    if (!s_sta_connected) {
        ESP_LOGW(TAG, "No WiFi connection, starting setup access point");
        start_ap();
    }
}

static void on_retry_timer(void *arg)
{
    if (s_has_credentials && !s_sta_connected) {
        esp_wifi_connect();
    }
}

static void on_ap_stop_timer(void *arg)
{
    if (s_sta_connected) {
        stop_ap();
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_has_credentials) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *event = (wifi_event_sta_disconnected_t *)data;
        if (s_sta_connected) {
            ESP_LOGW(TAG, "Disconnected from WiFi (reason %d)", event->reason);
        }
        s_sta_connected = false;
        if (s_has_credentials) {
            restart_timer(s_retry_timer, s_ap_active ? RETRY_DELAY_AP_US : RETRY_DELAY_US);
        }
        if (!s_ap_active && !esp_timer_is_active(s_fallback_timer)) {
            esp_timer_start_once(s_fallback_timer, FALLBACK_AP_DELAY_US);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Connected, IP " IPSTR ", http://%s.local/",
                 IP2STR(&event->ip_info.ip), CONFIG_SVS_HOSTNAME);
        s_sta_connected = true;
        esp_timer_stop(s_fallback_timer);
        esp_timer_stop(s_retry_timer);
        if (s_ap_active) {
            restart_timer(s_ap_stop_timer, AP_SHUTDOWN_DELAY_US);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void start()
{
    s_scan_mutex = xSemaphoreCreateMutex();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_netif, CONFIG_SVS_HOSTNAME));

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = on_fallback_timer;
    timer_args.name = "wifi_fallback";
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_fallback_timer));
    timer_args.callback = on_retry_timer;
    timer_args.name = "wifi_retry";
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));
    timer_args.callback = on_ap_stop_timer;
    timer_args.name = "wifi_ap_stop";
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_ap_stop_timer));

    // Credentials are kept by the WiFi driver itself in NVS
    wifi_config_t sta_config = {};
    ESP_ERROR_CHECK(esp_wifi_get_config(WIFI_IF_STA, &sta_config));
    s_has_credentials = sta_config.sta.ssid[0] != 0;

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "SVS-Bridge-%02X%02X", mac[4], mac[5]);
    s_ap_ssid = ssid;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Power save trades latency for current draw (SVS Bridge -> Network menu).
    // NONE keeps the web UI/HA snappiest; modem sleep saves power on battery.
#if CONFIG_SVS_WIFI_PS_MAX
    wifi_ps_type_t ps = WIFI_PS_MAX_MODEM;
#elif CONFIG_SVS_WIFI_PS_MIN
    wifi_ps_type_t ps = WIFI_PS_MIN_MODEM;
#else
    wifi_ps_type_t ps = WIFI_PS_NONE;
#endif
    ESP_ERROR_CHECK(esp_wifi_set_ps(ps));

    if (s_has_credentials) {
        ESP_LOGI(TAG, "Connecting to \"%s\"", (const char *)sta_config.sta.ssid);
        esp_timer_start_once(s_fallback_timer, FALLBACK_AP_DELAY_US);
    } else {
        ESP_LOGI(TAG, "No WiFi configured");
        start_ap();
    }

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_SVS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("SVS Bridge"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));

    // Home Assistant discovers the bridge through this service (zeroconf). The
    // TXT record carries the stable id and API version so the integration can
    // offer the device and set a unique_id before the user pastes a token.
    std::string id = device_id();
    mdns_txt_item_t txt[] = {
        {"id", (char *)id.c_str()},
        {"version", (char *)"1"},
    };
    ESP_ERROR_CHECK(mdns_service_add("SVS Bridge API", "_svsbridge", "_tcp", 443,
                                     txt, sizeof(txt) / sizeof(txt[0])));
}

std::string device_id()
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    char buf[24];
    snprintf(buf, sizeof(buf), "svs-bridge-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

bool sta_connected() { return s_sta_connected; }

bool ap_active() { return s_ap_active; }

std::string ap_ssid() { return s_ap_ssid; }

std::string sta_ssid()
{
    wifi_config_t config = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK) {
        return "";
    }
    return std::string((const char *)config.sta.ssid,
                       strnlen((const char *)config.sta.ssid, sizeof(config.sta.ssid)));
}

std::string sta_ip()
{
    esp_netif_ip_info_t ip_info;
    if (!s_sta_connected || esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK) {
        return "";
    }
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
    return buf;
}

int sta_rssi()
{
    wifi_ap_record_t info;
    if (!s_sta_connected || esp_wifi_sta_get_ap_info(&info) != ESP_OK) {
        return 0;
    }
    return info.rssi;
}

std::string scan_json()
{
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);

    // A connection attempt in progress blocks scanning; pause it
    if (!s_sta_connected) {
        esp_timer_stop(s_retry_timer);
        esp_wifi_disconnect();
    }

    std::vector<wifi_ap_record_t> records;
    if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        records.resize(count);
        if (count > 0 && esp_wifi_scan_get_ap_records(&count, records.data()) == ESP_OK) {
            records.resize(count);
        } else {
            records.clear();
        }
    } else {
        ESP_LOGW(TAG, "Scan failed");
    }

    if (s_has_credentials && !s_sta_connected) {
        restart_timer(s_retry_timer, RETRY_DELAY_US);
    }
    xSemaphoreGive(s_scan_mutex);

    // Strongest first, one entry per network name, hidden networks skipped
    std::sort(records.begin(), records.end(),
              [](const wifi_ap_record_t &a, const wifi_ap_record_t &b) { return a.rssi > b.rssi; });

    JsonDocument doc;
    JsonArray list = doc.to<JsonArray>();
    std::vector<std::string> seen;
    for (const auto &rec : records) {
        std::string ssid((const char *)rec.ssid, strnlen((const char *)rec.ssid, sizeof(rec.ssid)));
        if (ssid.empty() || std::find(seen.begin(), seen.end(), ssid) != seen.end()) {
            continue;
        }
        seen.push_back(ssid);
        JsonObject net = list.add<JsonObject>();
        net["ssid"] = ssid;
        net["rssi"] = rec.rssi;
        net["secure"] = rec.authmode != WIFI_AUTH_OPEN;
        if ((int)seen.size() >= MAX_SCAN_RESULTS) {
            break;
        }
    }

    std::string out;
    serializeJson(doc, out);
    return out;
}

esp_err_t set_credentials(const std::string &ssid, const std::string &password)
{
    wifi_config_t config = {};
    if (ssid.empty() || ssid.size() >= sizeof(config.sta.ssid) ||
        password.size() >= sizeof(config.sta.password)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(config.sta.ssid, ssid.data(), ssid.size());
    memcpy(config.sta.password, password.data(), password.size());

    esp_timer_stop(s_retry_timer);
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        return err;
    }
    s_has_credentials = true;
    ESP_LOGI(TAG, "New WiFi credentials saved, connecting to \"%s\"", ssid.c_str());
    return esp_wifi_connect();
}

}  // namespace wifi_manager
