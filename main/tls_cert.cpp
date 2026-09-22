#include "tls_cert.h"

#include <stdio.h>
#include <string.h>
#include <memory>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

namespace tls_cert {

static const char *TAG = "tls_cert";
static const char *NVS_NAMESPACE = "tls";
static const char *KEY_CERT = "cert";
static const char *KEY_KEY = "key";
static const char *KEY_SOURCE = "source";

static const size_t PEM_BUF_LEN = 2048;
static const char *SUBJECT = "CN=" CONFIG_SVS_HOSTNAME ".local,O=SVS Bridge";
static const char *SAN_LOCAL = CONFIG_SVS_HOSTNAME ".local";
static const char *SAN_HOST = CONFIG_SVS_HOSTNAME;
// The device has no clock at first boot, so use a fixed, wide validity window
static const char *NOT_BEFORE = "20260101000000";
static const char *NOT_AFTER = "20491231235959";

static std::string s_cert;
static std::string s_key;
static std::string s_fingerprint;
static Source s_source = Source::Generated;

// ---------------------------------------------------------------------------
// NVS
// ---------------------------------------------------------------------------

static esp_err_t nvs_read_blob(nvs_handle_t h, const char *key, std::string &out)
{
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, key, NULL, &len);
    if (err != ESP_OK) {
        return err;
    }
    out.resize(len);
    return nvs_get_blob(h, key, out.data(), &len);
}

static esp_err_t save(const std::string &cert, const std::string &key, Source source)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY_CERT, cert.data(), cert.size());
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY_KEY, key.data(), key.size());
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_SOURCE, source == Source::Custom ? "custom" : "generated");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// ---------------------------------------------------------------------------
// Crypto
// ---------------------------------------------------------------------------

// Parses a PEM certificate and computes its SHA-256 fingerprint.
static esp_err_t compute_fingerprint(const std::string &cert, std::string &out)
{
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    // PEM parsing needs the terminating '\0' included in the length
    int ret = mbedtls_x509_crt_parse(&crt, (const unsigned char *)cert.c_str(), cert.size() + 1);
    if (ret != 0) {
        mbedtls_x509_crt_free(&crt);
        ESP_LOGE(TAG, "Certificate parse failed: -0x%04X", (unsigned)-ret);
        return ESP_FAIL;
    }
    uint8_t hash[32];
    size_t hash_len = 0;
    psa_status_t status = psa_hash_compute(PSA_ALG_SHA_256, crt.raw.p, crt.raw.len,
                                           hash, sizeof(hash), &hash_len);
    mbedtls_x509_crt_free(&crt);
    if (status != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    out.clear();
    for (size_t i = 0; i < hash_len; i++) {
        char hex[4];
        snprintf(hex, sizeof(hex), i ? ":%02X" : "%02X", hash[i]);
        out += hex;
    }
    return ESP_OK;
}

// Generates an ECDSA P-256 key and a self-signed certificate for it.
static esp_err_t generate(std::string &cert, std::string &key)
{
    psa_key_attributes_t attr = psa_key_attributes_init();
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key_id;
    psa_status_t status = psa_generate_key(&attr, &key_id);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "Key generation failed: %d", (int)status);
        return ESP_FAIL;
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_copy_from_psa(key_id, &pk);
    psa_destroy_key(key_id);
    if (ret != 0) {
        mbedtls_pk_free(&pk);
        ESP_LOGE(TAG, "Key copy failed: -0x%04X", (unsigned)-ret);
        return ESP_FAIL;
    }

    uint8_t serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] = (serial[0] & 0x7F) | 0x01;  // positive and without leading zero

    mbedtls_x509_san_list san_host = {};
    san_host.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san_host.node.san.unstructured_name.p = (unsigned char *)SAN_HOST;
    san_host.node.san.unstructured_name.len = strlen(SAN_HOST);
    mbedtls_x509_san_list san_local = {};
    san_local.node.type = MBEDTLS_X509_SAN_DNS_NAME;
    san_local.node.san.unstructured_name.p = (unsigned char *)SAN_LOCAL;
    san_local.node.san.unstructured_name.len = strlen(SAN_LOCAL);
    san_local.next = &san_host;

    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &pk);
    mbedtls_x509write_crt_set_issuer_key(&crt, &pk);  // self-signed

    std::unique_ptr<unsigned char[]> buf(new unsigned char[PEM_BUF_LEN]);
    ret = mbedtls_x509write_crt_set_subject_name(&crt, SUBJECT);
    if (ret == 0) ret = mbedtls_x509write_crt_set_issuer_name(&crt, SUBJECT);
    if (ret == 0) ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    if (ret == 0) ret = mbedtls_x509write_crt_set_validity(&crt, NOT_BEFORE, NOT_AFTER);
    if (ret == 0) ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    if (ret == 0) ret = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    if (ret == 0) ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san_local);
    if (ret == 0) ret = mbedtls_x509write_crt_pem(&crt, buf.get(), PEM_BUF_LEN);
    if (ret == 0) {
        cert = (const char *)buf.get();
        ret = mbedtls_pk_write_key_pem(&pk, buf.get(), PEM_BUF_LEN);
    }
    if (ret == 0) {
        key = (const char *)buf.get();
    }

    memset(buf.get(), 0, PEM_BUF_LEN);  // wipe the private key copy
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);

    if (ret != 0) {
        ESP_LOGE(TAG, "Certificate generation failed: -0x%04X", (unsigned)-ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

esp_err_t load()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        bool ok = nvs_read_blob(h, KEY_CERT, s_cert) == ESP_OK &&
                  nvs_read_blob(h, KEY_KEY, s_key) == ESP_OK;
        char source[16] = {};
        size_t source_len = sizeof(source);
        if (nvs_get_str(h, KEY_SOURCE, source, &source_len) == ESP_OK) {
            s_source = strcmp(source, "custom") == 0 ? Source::Custom : Source::Generated;
        }
        nvs_close(h);
        if (ok && compute_fingerprint(s_cert, s_fingerprint) == ESP_OK) {
            ESP_LOGI(TAG, "Loaded %s certificate, SHA-256 %s",
                     s_source == Source::Custom ? "custom" : "self-signed", s_fingerprint.c_str());
            return ESP_OK;
        }
        if (ok) {
            ESP_LOGW(TAG, "Stored certificate is unreadable, generating a new one");
        }
    }

    ESP_LOGI(TAG, "Generating self-signed certificate for %s", SAN_LOCAL);
    esp_err_t err = generate(s_cert, s_key);
    if (err != ESP_OK) {
        return err;
    }
    s_source = Source::Generated;
    err = save(s_cert, s_key, s_source);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not store certificate: %s", esp_err_to_name(err));
        return err;
    }
    err = compute_fingerprint(s_cert, s_fingerprint);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "New certificate, SHA-256 %s", s_fingerprint.c_str());
    }
    return err;
}

const std::string &cert_pem() { return s_cert; }

const std::string &key_pem() { return s_key; }

const std::string &fingerprint() { return s_fingerprint; }

Source source() { return s_source; }

}  // namespace tls_cert
