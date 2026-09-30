// /api/v1/events' event types: which ones a socket gets, from its ?types= list, and their names for
// its "hello". The same contract as Cruller's /api/v1/events, so both integrations read them alike.
// Pure: no ESP-IDF, no I/O, no globals (see tests/test_api_events.cpp).
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace api_events {

// Types, as bits of a mask. A socket gets only the types it names in ?types=, so a type added later
// never reaches a client that didn't ask for it.
constexpr uint32_t STATE = 1u << 0;  // "state": the /api/v1/state JSON
constexpr uint32_t ALL = STATE;

constexpr const char *NAMES[] = {"state"};  // bit i is NAMES[i]
constexpr size_t COUNT = sizeof(NAMES) / sizeof(NAMES[0]);

// A query value as sent: %XX and '+' decoded ("state%2Clater" -> "state,later").
inline std::string url_decode(const std::string &in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); i++) {
        const char c = in[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) &&
                   isxdigit((unsigned char)in[i + 2])) {
            out += (char)strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

// The types named in a comma-separated list ("state,later"; spaces around names allowed), names the
// bridge doesn't have left out.
inline uint32_t parse(const std::string &list)
{
    uint32_t types = 0;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(',', start);
        if (end == std::string::npos) {
            end = list.size();
        }
        size_t a = start, b = end;
        while (a < b && list[a] == ' ') {
            a++;
        }
        while (b > a && list[b - 1] == ' ') {
            b--;
        }
        const std::string name = list.substr(a, b - a);
        for (size_t i = 0; i < COUNT; i++) {
            if (!name.empty() && name == NAMES[i]) {
                types |= 1u << i;
            }
        }
        start = end + 1;
    }
    return types;
}

// The names of the types in a mask, as a JSON array (["state"]; "[]" for none).
inline std::string names_json(uint32_t types)
{
    std::string out = "[";
    for (size_t i = 0; i < COUNT; i++) {
        if (types & (1u << i)) {
            out += out.size() > 1 ? ",\"" : "\"";
            out += NAMES[i];
            out += '"';
        }
    }
    return out + "]";
}

}  // namespace api_events
