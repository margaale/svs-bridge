// Parsing of the SVS's serial output.
//
// The SVS prints its banner on boot and repeats status lines every ~2 s; an
// input change is announced with "SVS NEW INPUT=n". Detecting that change is
// the whole point of the Home Assistant integration, so the pure parsing lives
// here and is unit-tested on the host (see test/host/test_svs_protocol.cpp).
// Pure: no ESP-IDF, no I/O, no globals.
#pragma once

#include <cstdlib>
#include <string>

namespace svs_protocol {

// What a (printable-stripped) line from the SVS means.
enum class LineKind { None, Firmware, InputChange, TotalInputs };

struct ParsedLine {
    LineKind kind = LineKind::None;
    int value = -1;  // input number for InputChange/TotalInputs, else -1
};

// The last run of digits in a line, or -1 if there is none. "SVS NEW INPUT=4"
// -> 4; the official utility reads the active input the same way.
inline int trailing_number(const std::string &line)
{
    size_t end = line.find_last_of("0123456789");
    if (end == std::string::npos) {
        return -1;
    }
    size_t start = line.find_last_not_of("0123456789", end);
    start = start == std::string::npos ? 0 : start + 1;
    return atoi(line.substr(start, end - start + 1).c_str());
}

// Classifies a line from the SVS. Matches the exact prefixes the SVS emits.
inline ParsedLine parse_svs_line(const std::string &line)
{
    if (line.rfind("SVS_FW_", 0) == 0) {
        return {LineKind::Firmware, -1};
    }
    // "SVS NEW ..." follows every input change; "SVS CURRENT ..." is the 2 s
    // heartbeat's copy of the same value.
    if (line.rfind("SVS CURRENT", 0) == 0 || line.rfind("SVS NEW", 0) == 0) {
        return {LineKind::InputChange, trailing_number(line)};
    }
    if (line.rfind("SVS TOTAL", 0) == 0) {
        return {LineKind::TotalInputs, trailing_number(line)};
    }
    return {LineKind::None, -1};
}

// True if the line only holds control characters, shown as <XX> (the SVS sends
// a lone 0x1A after its version, which the official utility strips).
inline bool only_control_chars(const std::string &line)
{
    for (size_t i = 0; i < line.size();) {
        if (line[i] == '<' && i + 3 < line.size() && line[i + 3] == '>') {
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

// De-duplicates the SVS's repeated "SVS TOTAL INPUTS=n"/"SVS CURRENT INPUT=n"
// heartbeat so the web log keeps them only when their value changes. Stateful:
// keep one instance per stream. A firmware banner resets it (the SVS rebooted).
struct StatusFilter {
    std::string last_total, last_current;

    bool is_repeated(const std::string &line)
    {
        if (line.rfind("SVS_FW_", 0) == 0) {
            last_total.clear();
            last_current.clear();
            return false;
        }
        std::string *last = line.rfind("SVS TOTAL", 0) == 0     ? &last_total
                            : line.rfind("SVS CURRENT", 0) == 0 ? &last_current
                                                                : nullptr;
        if (last == nullptr) {
            return false;
        }
        if (*last == line) {
            return true;
        }
        *last = line;
        return false;
    }
};

}  // namespace svs_protocol
