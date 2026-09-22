// Admin password and web UI sessions.
//
// There is a single user, "admin". Its password is stored in NVS (namespace
// "auth") as a salted PBKDF2-HMAC-SHA256 hash, never in clear. A factory
// reset erases it, which is the way to recover a forgotten password.
//
// Sessions live in RAM only: logging in again is needed after a reboot.
#pragma once

#include <string>
#include "esp_err.h"

namespace auth {

static const size_t MIN_PASSWORD_LEN = 8;
static const size_t MAX_PASSWORD_LEN = 64;

// Reads the stored password hash. Requires NVS to be initialized.
esp_err_t load();

bool password_set();

// Stores a new password. ESP_ERR_INVALID_ARG if its length is out of range.
esp_err_t set_password(const std::string &password);

// Checks a login attempt. Consecutive failures lock logins for a while; while
// locked every attempt fails and *retry_after_s says for how long.
enum class LoginResult { Ok, WrongPassword, Locked };
LoginResult check_password(const std::string &password, int *retry_after_s);

// Returns a new session token, dropping the oldest session if the table is full.
std::string create_session();
bool session_valid(const std::string &token);  // also extends its lifetime
void end_session(const std::string &token);
void end_all_sessions();

// API token for external integrations (Home Assistant): a 64-hex-char random
// string, Bearer-authenticated and separate from the admin password. It is
// generated on first use and kept in NVS; a factory reset clears it.
std::string api_token();              // current token, generating one if none exists
std::string regenerate_api_token();   // a fresh token, revoking the old one
bool api_token_valid(const std::string &token);  // constant-time check

}  // namespace auth
