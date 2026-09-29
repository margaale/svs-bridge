#include "bridge_fw_repo.h"

#include <stdio.h>
#include <string.h>
#include <algorithm>
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
    "https://api.github.com/repos/margaale/svs-bridge/releases?per_page=30";  // alphas add up
// The OTA image: "svs-bridge-<version>-<board>-svs_bridge.bin" (CI names it so), or plain
// "svs_bridge.bin" as on earlier releases.
static const char *ASSET_NAME = "svs_bridge.bin";

// GitHub allows 60 unauthenticated calls per hour, so a listing is reused briefly.
static const int64_t CACHE_US = 60LL * 1000 * 1000;
static std::vector<Release> s_releases;
static int64_t s_listed_at = 0;

static bool is_image(const std::string &name)
{
    const std::string plain(ASSET_NAME), suffix = std::string("-") + ASSET_NAME;
    return name == plain ||
           (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0);
}

// Feeds an HTTP answer to ArduinoJson as it arrives. The release list runs to
// hundreds of KB (every asset carries its uploader's whole profile) and grows
// with each release, but the filter keeps only a few fields, so the answer is
// never held in memory.
class HttpReader {
public:
    explicit HttpReader(esp_http_client_handle_t client) : client_(client), buf_(new char[CHUNK]) {}

    int read()
    {
        return fill() ? (unsigned char)buf_[pos_++] : -1;
    }

    size_t readBytes(char *out, size_t length)
    {
        size_t done = 0;
        while (done < length && fill()) {
            size_t take = std::min(length - done, (size_t)(len_ - pos_));
            memcpy(out + done, buf_.get() + pos_, take);
            pos_ += take;
            done += take;
        }
        return done;
    }

    bool failed() const { return failed_; }

private:
    static const int CHUNK = 2048;

    bool fill()
    {
        if (pos_ < len_) return true;
        if (failed_) return false;
        len_ = esp_http_client_read(client_, buf_.get(), CHUNK);
        pos_ = 0;
        if (len_ < 0) {
            failed_ = true;
            len_ = 0;
        }
        return len_ > 0;
    }

    esp_http_client_handle_t client_;
    std::unique_ptr<char[]> buf_;
    int len_ = 0, pos_ = 0;
    bool failed_ = false;
};

// Sends a GET and checks the status. On success the body is ready to read,
// and the caller closes the client with close_get().
static esp_err_t open_get(const char *url, esp_http_client_handle_t &out, std::string *error)
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
            *error = "No releases found on GitHub";
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "GitHub answered HTTP %d", status);
            *error = msg;
        }
        return ESP_FAIL;
    }
    out = client;
    return ESP_OK;
}

static void close_get(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

esp_err_t list(std::vector<Release> &out, std::string *error)
{
    if (s_listed_at != 0 && esp_timer_get_time() - s_listed_at < CACHE_US) {
        out = s_releases;
        return ESP_OK;
    }

    esp_http_client_handle_t client;
    esp_err_t err = open_get(LIST_URL, client, error);
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
    HttpReader reader(client);
    DeserializationError parsed = deserializeJson(doc, reader, DeserializationOption::Filter(filter));
    close_get(client);
    if (reader.failed()) {
        *error = "Download interrupted";
        return ESP_FAIL;
    }
    if (parsed != DeserializationError::Ok || !doc.is<JsonArray>()) {
        ESP_LOGE(TAG, "Release list: %s", parsed.c_str());
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
            if (is_image(asset["name"] | "")) {
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
