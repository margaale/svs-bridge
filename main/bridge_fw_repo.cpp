#include "bridge_fw_repo.h"

#include <stdio.h>
#include <memory>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ArduinoJson.h"

namespace bridge_fw_repo {

static const char *TAG = "bridge_fw_repo";

// The bridge's own repository. Public releases are read anonymously.
static const char *LIST_URL =
    "https://api.github.com/repos/margaale/svs-bridge/releases?per_page=15";
static const char *ASSET_NAME = "svs_bridge.bin";
static const size_t MAX_LIST_BYTES = 256 * 1024;  // release notes can be large

// GitHub allows 60 unauthenticated calls per hour, so a listing is reused briefly.
static const int64_t CACHE_US = 60LL * 1000 * 1000;
static std::vector<Release> s_releases;
static int64_t s_listed_at = 0;

static esp_err_t http_get(const char *url, std::string &body, std::string *error)
{
    esp_http_client_config_t config = {};
    config.url = url;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 15000;
    config.buffer_size = 2048;
    config.user_agent = "svs-bridge";  // GitHub rejects requests without one

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        *error = "Out of memory";
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        *error = "Could not reach GitHub (is the bridge online?)";
        return err;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (status == 404) {
            *error = "No releases found (is the firmware repository public?)";
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "GitHub answered HTTP %d", status);
            *error = msg;
        }
        return ESP_FAIL;
    }

    body.clear();
    const int chunk = 2048;
    std::unique_ptr<char[]> buf(new char[chunk]);
    while (true) {
        int n = esp_http_client_read(client, buf.get(), chunk);
        if (n < 0) { err = ESP_FAIL; *error = "Download interrupted"; break; }
        if (n == 0) break;
        if (body.size() + n > MAX_LIST_BYTES) { err = ESP_ERR_INVALID_SIZE; *error = "Answer larger than expected"; break; }
        body.append(buf.get(), n);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t list(std::vector<Release> &out, std::string *error)
{
    if (s_listed_at != 0 && esp_timer_get_time() - s_listed_at < CACHE_US) {
        out = s_releases;
        return ESP_OK;
    }

    std::string body;
    esp_err_t err = http_get(LIST_URL, body, error);
    if (err != ESP_OK) {
        return err;
    }

    JsonDocument filter;
    JsonObject f = filter[0].to<JsonObject>();
    f["tag_name"] = true;
    f["name"] = true;
    f["html_url"] = true;
    f["prerelease"] = true;
    JsonObject fa = f["assets"][0].to<JsonObject>();
    fa["name"] = true;
    fa["size"] = true;
    fa["browser_download_url"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok ||
        !doc.is<JsonArray>()) {
        *error = "Unexpected answer from GitHub";
        return ESP_FAIL;
    }

    std::vector<Release> found;
    for (JsonObject rel : doc.as<JsonArray>()) {
        Release r;
        r.tag = rel["tag_name"] | "";
        r.name = rel["name"] | "";
        r.notes_url = rel["html_url"] | "";
        r.prerelease = rel["prerelease"] | false;
        for (JsonObject asset : rel["assets"].as<JsonArray>()) {
            if ((asset["name"] | "") == std::string(ASSET_NAME)) {
                r.size = asset["size"] | 0;
                r.url = asset["browser_download_url"] | "";
                break;
            }
        }
        if (r.tag.empty() || r.url.empty()) {
            continue;  // no flashable asset on this release
        }
        found.push_back(std::move(r));
    }

    s_releases = found;  // GitHub already returns releases newest first
    s_listed_at = esp_timer_get_time();
    out = found;
    ESP_LOGI(TAG, "%u bridge releases with a firmware asset", (unsigned)out.size());
    return ESP_OK;
}

bool resolve(const std::string &tag, std::string &url, size_t &size)
{
    for (const auto &r : s_releases) {
        if (r.tag == tag) {
            url = r.url;
            size = r.size;
            return true;
        }
    }
    return false;
}

}  // namespace bridge_fw_repo
