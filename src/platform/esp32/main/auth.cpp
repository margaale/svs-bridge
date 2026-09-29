#include "auth.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "psa/crypto.h"

namespace auth {

static const char *TAG = "auth";
static const char *NVS_NAMESPACE = "auth";
static const char *KEY_SALT = "salt";
static const char *KEY_HASH = "hash";
static const char *KEY_ITERATIONS = "iter";
static const char *KEY_API_TOKEN = "apitok";

static const size_t API_TOKEN_BYTES = 32;  // 64 hex chars

static const size_t SALT_LEN = 16;
static const size_t HASH_LEN = 32;
static const uint32_t PBKDF2_ITERATIONS = 10000;

static const int MAX_SESSIONS = 4;
static const size_t TOKEN_BYTES = 32;
static const int64_t SESSION_IDLE_US = 12LL * 3600 * 1000 * 1000;  // 12 h without use

static const int FREE_ATTEMPTS = 5;         // failures before logins get locked
static const int LOCK_BASE_S = 30;          // first lock, doubled on each further failure
static const int LOCK_MAX_S = 15 * 60;

struct Session {
    std::string token;
    int64_t last_used_us = 0;
};

static SemaphoreHandle_t s_mutex = nullptr;
static bool s_password_set = false;
static uint8_t s_salt[SALT_LEN];
static uint8_t s_hash[HASH_LEN];
static uint32_t s_iterations = PBKDF2_ITERATIONS;
static Session s_sessions[MAX_SESSIONS];
static int s_failures = 0;
static int64_t s_locked_until_us = 0;
static std::string s_api_token;  // cached; loaded on demand, generated if absent

static int64_t now_us() { return esp_timer_get_time(); }

// Hex encoding of `len` cryptographically random bytes (2 chars per byte).
static std::string random_hex(size_t len)
{
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        uint8_t b = (uint8_t)(esp_random() & 0xFF);
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", b);
        out += hex;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------

static esp_err_t pbkdf2(const std::string &password, const uint8_t *salt, uint32_t iterations,
                        uint8_t *out)
{
    psa_key_derivation_operation_t op = psa_key_derivation_operation_init();
    psa_status_t st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iterations);
    }
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, SALT_LEN);
    }
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                            (const uint8_t *)password.data(), password.size());
    }
    if (st == PSA_SUCCESS) {
        st = psa_key_derivation_output_bytes(&op, out, HASH_LEN);
    }
    psa_key_derivation_abort(&op);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "PBKDF2 failed: %d", (int)st);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Constant time, so response timing does not leak how many bytes matched
static bool equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

// ---------------------------------------------------------------------------
// Password
// ---------------------------------------------------------------------------

esp_err_t load()
{
    if (s_mutex == nullptr) {
        s_mutex = xSemaphoreCreateMutex();
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        s_password_set = false;  // namespace not created yet: no password
        ESP_LOGI(TAG, "No admin password set");
        return ESP_OK;
    }
    size_t salt_len = SALT_LEN;
    size_t hash_len = HASH_LEN;
    bool ok = nvs_get_blob(h, KEY_SALT, s_salt, &salt_len) == ESP_OK && salt_len == SALT_LEN &&
              nvs_get_blob(h, KEY_HASH, s_hash, &hash_len) == ESP_OK && hash_len == HASH_LEN &&
              nvs_get_u32(h, KEY_ITERATIONS, &s_iterations) == ESP_OK;
    nvs_close(h);
    s_password_set = ok;
    ESP_LOGI(TAG, "%s", ok ? "Admin password loaded" : "No admin password set");
    return ESP_OK;
}

bool password_set() { return s_password_set; }

esp_err_t set_password(const std::string &password)
{
    if (password.size() < MIN_PASSWORD_LEN || password.size() > MAX_PASSWORD_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t salt[SALT_LEN];
    uint8_t hash[HASH_LEN];
    esp_fill_random(salt, sizeof(salt));
    esp_err_t err = pbkdf2(password, salt, PBKDF2_ITERATIONS, hash);
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t h;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY_SALT, salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_set_blob(h, KEY_HASH, hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_set_u32(h, KEY_ITERATIONS, PBKDF2_ITERATIONS);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not store password: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(s_salt, salt, sizeof(salt));
    memcpy(s_hash, hash, sizeof(hash));
    s_iterations = PBKDF2_ITERATIONS;
    s_password_set = true;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Admin password set");
    return ESP_OK;
}

LoginResult check_password(const std::string &password, int *retry_after_s)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int64_t now = now_us();
    if (now < s_locked_until_us) {
        *retry_after_s = (int)((s_locked_until_us - now) / 1000000) + 1;
        xSemaphoreGive(s_mutex);
        return LoginResult::Locked;
    }
    uint8_t salt[SALT_LEN];
    uint8_t expected[HASH_LEN];
    memcpy(salt, s_salt, sizeof(salt));
    memcpy(expected, s_hash, sizeof(expected));
    uint32_t iterations = s_iterations;
    bool is_set = s_password_set;
    xSemaphoreGive(s_mutex);

    uint8_t hash[HASH_LEN];
    bool ok = is_set && password.size() <= MAX_PASSWORD_LEN &&
              pbkdf2(password, salt, iterations, hash) == ESP_OK && equal(hash, expected, HASH_LEN);

    LoginResult result = LoginResult::Ok;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (ok) {
        s_failures = 0;
    } else {
        s_failures++;
        result = LoginResult::WrongPassword;
        if (s_failures >= FREE_ATTEMPTS) {
            int shift = s_failures - FREE_ATTEMPTS;
            int lock_s = shift >= 5 ? LOCK_MAX_S : LOCK_BASE_S << shift;
            if (lock_s > LOCK_MAX_S) {
                lock_s = LOCK_MAX_S;
            }
            s_locked_until_us = now_us() + (int64_t)lock_s * 1000000;
            *retry_after_s = lock_s;
            result = LoginResult::Locked;
            ESP_LOGW(TAG, "%d failed logins, locked for %d s", s_failures, lock_s);
        }
    }
    xSemaphoreGive(s_mutex);
    return result;
}

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

std::string create_session()
{
    uint8_t raw[TOKEN_BYTES];
    esp_fill_random(raw, sizeof(raw));
    std::string token;
    for (uint8_t b : raw) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", b);
        token += hex;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    // Reuse a free or expired slot, otherwise evict the least recently used
    int64_t now = now_us();
    int slot = 0;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        bool free = s_sessions[i].token.empty() || now - s_sessions[i].last_used_us > SESSION_IDLE_US;
        if (free) {
            slot = i;
            break;
        }
        if (s_sessions[i].last_used_us < s_sessions[slot].last_used_us) {
            slot = i;
        }
    }
    s_sessions[slot].token = token;
    s_sessions[slot].last_used_us = now;
    xSemaphoreGive(s_mutex);
    return token;
}

bool session_valid(const std::string &token)
{
    if (token.size() != TOKEN_BYTES * 2) {
        return false;
    }
    bool valid = false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int64_t now = now_us();
    for (auto &s : s_sessions) {
        if (s.token.size() == token.size() &&
            equal((const uint8_t *)s.token.data(), (const uint8_t *)token.data(), token.size())) {
            if (now - s.last_used_us <= SESSION_IDLE_US) {
                s.last_used_us = now;
                valid = true;
            } else {
                s.token.clear();  // expired
            }
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return valid;
}

void end_session(const std::string &token)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (auto &s : s_sessions) {
        if (s.token == token) {
            s.token.clear();
        }
    }
    xSemaphoreGive(s_mutex);
}

void end_all_sessions()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (auto &s : s_sessions) {
        s.token.clear();
    }
    xSemaphoreGive(s_mutex);
}

// ---------------------------------------------------------------------------
// API token (Bearer auth for external integrations)
// ---------------------------------------------------------------------------

// Persists `token` under KEY_API_TOKEN. Returns ESP_OK on success.
static esp_err_t store_api_token(const std::string &token)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, KEY_API_TOKEN, token.c_str());
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

std::string api_token()
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_api_token.empty()) {
        // Try to load a previously stored token before minting a new one.
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
            char buf[API_TOKEN_BYTES * 2 + 1];
            size_t len = sizeof(buf);
            if (nvs_get_str(h, KEY_API_TOKEN, buf, &len) == ESP_OK && len == sizeof(buf)) {
                s_api_token.assign(buf, sizeof(buf) - 1);
            }
            nvs_close(h);
        }
        if (s_api_token.empty()) {
            std::string token = random_hex(API_TOKEN_BYTES);
            if (store_api_token(token) == ESP_OK) {
                s_api_token = token;
                ESP_LOGI(TAG, "Generated API token");
            } else {
                ESP_LOGE(TAG, "Could not store API token");
                // Return it anyway for this boot rather than nothing.
                s_api_token = token;
            }
        }
    }
    std::string token = s_api_token;
    xSemaphoreGive(s_mutex);
    return token;
}

std::string regenerate_api_token()
{
    std::string token = random_hex(API_TOKEN_BYTES);
    esp_err_t err = store_api_token(token);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (err == ESP_OK) {
        s_api_token = token;
        ESP_LOGI(TAG, "Regenerated API token");
    }
    std::string current = s_api_token;
    xSemaphoreGive(s_mutex);
    return err == ESP_OK ? token : current;
}

bool api_token_valid(const std::string &candidate)
{
    if (candidate.size() != API_TOKEN_BYTES * 2) {
        return false;  // wrong length: no need to touch the real token
    }
    std::string token = api_token();  // ensures one exists
    if (token.size() != candidate.size()) {
        return false;
    }
    return equal((const uint8_t *)token.data(), (const uint8_t *)candidate.data(), token.size());
}

}  // namespace auth
