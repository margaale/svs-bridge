// TLS certificate for the HTTPS server.
//
// The certificate and private key live in NVS (namespace "tls") so they
// survive OTA updates. On first boot a self-signed ECDSA P-256 certificate is
// generated on the device; the private key never leaves it.
//
// Storage is source-agnostic ("cert", "key" and "source" entries) so a
// user-provided certificate can replace the generated one later on.
#pragma once

#include <string>
#include "esp_err.h"

namespace tls_cert {

enum class Source { Generated, Custom };

// Loads the certificate from NVS, generating a new one if there is none or
// the stored one is unreadable. Requires NVS to be initialized.
esp_err_t load();

// PEM strings (without the trailing '\0')
const std::string &cert_pem();
const std::string &key_pem();

// SHA-256 of the DER certificate as "AB:CD:...", for display and pinning
const std::string &fingerprint();

Source source();

}  // namespace tls_cert
