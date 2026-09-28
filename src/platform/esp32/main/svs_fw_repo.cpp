#include "svs_fw_repo.h"

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <memory>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "ArduinoJson.h"

namespace svs_fw_repo {

static const char *TAG = "svs_fw_repo";

static const char *LIST_URL =
    "https://api.github.com/repos/Arthrimus/SVS_Firmware_Repository/contents/"
    "SVS%20Control%20Module%20Firmwares/Firmware";
static const size_t MAX_LIST_BYTES = 128 * 1024;
static const size_t MAX_HEX_BYTES = 256 * 1024;

struct Entry {
    Release release;
    std::string url;  // download_url
    std::string sha;  // git blob SHA-1, hex
};

// The web UI asks for the list whenever the releases menu is opened. GitHub
// allows 60 unauthenticated API calls per hour, so a listing is reused briefly.
static const int64_t CACHE_US = 60LL * 1000 * 1000;

// Last listing, so download() only fetches files the repository listed
static std::vector<Entry> s_entries;
static int64_t s_listed_at = 0;  // 0 = never

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

static esp_err_t http_get(const char *url, size_t max_bytes, std::string &body, std::string *error)
{
    esp_http_client_config_t config = {};
    config.url = url;
    config.crt_bundle_attach = esp_crt_bundle_attach;  // verify github.com against the CA bundle
    config.timeout_ms = 15000;
    config.buffer_size = 2048;
    config.user_agent = "svs-bridge";  // GitHub's API rejects requests without one

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
        char msg[64];
        snprintf(msg, sizeof(msg), "GitHub answered HTTP %d", status);
        *error = msg;
        return ESP_FAIL;
    }

    body.clear();
    const int chunk = 2048;
    std::unique_ptr<char[]> buf(new char[chunk]);
    while (true) {
        int n = esp_http_client_read(client, buf.get(), chunk);
        if (n < 0) {
            err = ESP_FAIL;
            *error = "Download interrupted";
            break;
        }
        if (n == 0) {
            break;
        }
        if (body.size() + n > max_bytes) {
            err = ESP_ERR_INVALID_SIZE;
            *error = "Download larger than expected";
            break;
        }
        body.append(buf.get(), n);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// git hashes a file as SHA-1("blob <size>\0" + content)
static std::string git_blob_sha1(const std::string &content)
{
    char header[32];
    int header_len = snprintf(header, sizeof(header), "blob %u", (unsigned)content.size()) + 1;

    psa_hash_operation_t op = psa_hash_operation_init();
    uint8_t hash[20];
    size_t hash_len = 0;
    bool ok = psa_hash_setup(&op, PSA_ALG_SHA_1) == PSA_SUCCESS &&
              psa_hash_update(&op, (const uint8_t *)header, header_len) == PSA_SUCCESS &&
              psa_hash_update(&op, (const uint8_t *)content.data(), content.size()) == PSA_SUCCESS &&
              psa_hash_finish(&op, hash, sizeof(hash), &hash_len) == PSA_SUCCESS;
    psa_hash_abort(&op);
    if (!ok) {
        return "";
    }
    std::string out;
    for (size_t i = 0; i < hash_len; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", hash[i]);
        out += hex;
    }
    return out;
}

// "SVS_FW_1.20_BETA.hex" -> version "1.20 BETA", beta, sort key 1*1000+20
static bool parse_name(const std::string &name, Release &rel, int &key)
{
    const char *prefix = "SVS_FW_";
    const char *suffix = ".hex";
    size_t plen = strlen(prefix), slen = strlen(suffix);
    if (name.size() <= plen + slen || name.compare(0, plen, prefix) != 0 ||
        name.compare(name.size() - slen, slen, suffix) != 0) {
        return false;
    }
    std::string version = name.substr(plen, name.size() - plen - slen);
    int major = 0, minor = 0;
    if (sscanf(version.c_str(), "%d.%d", &major, &minor) != 2) {
        return false;
    }
    std::replace(version.begin(), version.end(), '_', ' ');
    rel.name = name;
    rel.version = version;
    rel.beta = version.find("BETA") != std::string::npos;
    // Newest first; a release sorts above the beta of the same version
    key = (major * 1000 + minor) * 2 + (rel.beta ? 0 : 1);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t list(std::vector<Release> &out, std::string *error)
{
    if (s_listed_at != 0 && esp_timer_get_time() - s_listed_at < CACHE_US) {
        out.clear();
        for (const auto &e : s_entries) {
            out.push_back(e.release);
        }
        return ESP_OK;
    }

    std::string body;
    esp_err_t err = http_get(LIST_URL, MAX_LIST_BYTES, body, error);
    if (err != ESP_OK) {
        return err;
    }

    // Only keep the fields we use
    JsonDocument filter;
    filter[0]["name"] = true;
    filter[0]["size"] = true;
    filter[0]["sha"] = true;
    filter[0]["download_url"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter)) != DeserializationError::Ok ||
        !doc.is<JsonArray>()) {
        *error = "Unexpected answer from GitHub";
        return ESP_FAIL;
    }

    std::vector<std::pair<int, Entry>> found;
    for (JsonObject item : doc.as<JsonArray>()) {
        Entry e;
        int key = 0;
        if (!parse_name(item["name"] | "", e.release, key)) {
            continue;
        }
        e.release.size = item["size"] | 0;
        e.url = item["download_url"] | "";
        e.sha = item["sha"] | "";
        // Only accept downloads from GitHub's raw content host
        if (e.url.rfind("https://raw.githubusercontent.com/", 0) != 0 || e.sha.size() != 40) {
            continue;
        }
        found.emplace_back(key, e);
    }
    std::sort(found.begin(), found.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });

    s_entries.clear();
    out.clear();
    for (auto &f : found) {
        out.push_back(f.second.release);
        s_entries.push_back(std::move(f.second));
    }
    s_listed_at = esp_timer_get_time();
    ESP_LOGI(TAG, "%u firmware files in the official repository", (unsigned)out.size());
    return ESP_OK;
}

esp_err_t download(const std::string &name, std::string &hex, std::string *error)
{
    auto it = std::find_if(s_entries.begin(), s_entries.end(),
                           [&](const Entry &e) { return e.release.name == name; });
    if (it == s_entries.end()) {
        *error = "Unknown firmware file; refresh the list of releases";
        return ESP_ERR_NOT_FOUND;
    }
    Entry entry = *it;

    esp_err_t err = http_get(entry.url.c_str(), MAX_HEX_BYTES, hex, error);
    if (err != ESP_OK) {
        return err;
    }
    std::string sha = git_blob_sha1(hex);
    if (sha != entry.sha) {
        ESP_LOGE(TAG, "%s: SHA-1 %s, GitHub lists %s", name.c_str(), sha.c_str(), entry.sha.c_str());
        *error = "The download does not match the file listed by GitHub";
        hex.clear();
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "Downloaded %s (%u bytes, git SHA-1 verified)", name.c_str(), (unsigned)hex.size());
    return ESP_OK;
}

}  // namespace svs_fw_repo
