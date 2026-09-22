// Intel HEX decoding for SVS firmware images.
//
// Turns an uploaded/downloaded .hex file (untrusted input) into the flash image
// that will be written to the SVS, validating every record's length, checksum,
// type and address range. A bug here could flash garbage, so it is unit-tested
// on the host (see test/host/test_svs_hex.cpp). Pure: no ESP-IDF, no I/O.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace svs_hex {

inline int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Decodes an Intel HEX file into a flash image no larger than max_size bytes,
// checking every record. Returns false and sets *error without producing an
// image on any malformed input.
inline bool decode_hex(const char *hex, size_t len, std::vector<uint8_t> &image,
                       size_t max_size, std::string *error)
{
    std::vector<uint8_t> flash(max_size, 0xFF);
    size_t top = 0;  // one past the highest written address
    uint32_t base = 0;
    bool eof = false;
    int line_no = 0;
    size_t pos = 0;
    char msg[96];

    while (pos < len && !eof) {
        size_t end = pos;
        while (end < len && hex[end] != '\n') {
            end++;
        }
        const char *line = hex + pos;
        size_t n = end - pos;
        pos = end + 1;
        line_no++;
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) {
            n--;
        }
        if (n == 0) {
            continue;
        }

        if (line[0] != ':' || n < 11 || (n - 1) % 2 != 0) {
            snprintf(msg, sizeof(msg), "Line %d is not a valid Intel HEX record", line_no);
            *error = msg;
            return false;
        }
        uint8_t rec[260];
        size_t rec_len = (n - 1) / 2;
        if (rec_len > sizeof(rec)) {
            snprintf(msg, sizeof(msg), "Line %d is too long", line_no);
            *error = msg;
            return false;
        }
        uint8_t sum = 0;
        for (size_t i = 0; i < rec_len; i++) {
            int hi = hex_nibble(line[1 + 2 * i]);
            int lo = hex_nibble(line[2 + 2 * i]);
            if (hi < 0 || lo < 0) {
                snprintf(msg, sizeof(msg), "Line %d has invalid characters", line_no);
                *error = msg;
                return false;
            }
            rec[i] = (uint8_t)(hi << 4 | lo);
            sum += rec[i];
        }
        uint8_t count = rec[0];
        if (rec_len != (size_t)count + 5) {
            snprintf(msg, sizeof(msg), "Line %d has a wrong length", line_no);
            *error = msg;
            return false;
        }
        if (sum != 0) {
            snprintf(msg, sizeof(msg), "Line %d has a wrong checksum (corrupted file?)", line_no);
            *error = msg;
            return false;
        }
        uint16_t addr = (uint16_t)(rec[1] << 8 | rec[2]);
        uint8_t type = rec[3];
        const uint8_t *data = rec + 4;

        bool address_record = type == 0x02 || type == 0x04;
        if (address_record && count != 2) {
            snprintf(msg, sizeof(msg), "Line %d has a malformed address record", line_no);
            *error = msg;
            return false;
        }

        switch (type) {
        case 0x00: {  // data
            uint32_t start = base + addr;
            if (start + count > max_size) {
                snprintf(msg, sizeof(msg), "Line %d writes past the ATmega328P flash (0x%05X)",
                         line_no, (unsigned)(start + count));
                *error = msg;
                return false;
            }
            memcpy(&flash[start], data, count);
            if (start + count > top) {
                top = start + count;
            }
            break;
        }
        case 0x01:  // end of file
            eof = true;
            break;
        case 0x02:  // extended segment address
            base = (uint32_t)(data[0] << 8 | data[1]) << 4;
            break;
        case 0x04:  // extended linear address
            base = (uint32_t)(data[0] << 8 | data[1]) << 16;
            break;
        case 0x03:  // start segment address
        case 0x05:  // start linear address: irrelevant for flashing
            break;
        default:
            snprintf(msg, sizeof(msg), "Line %d has an unsupported record (type 0x%02X)", line_no, type);
            *error = msg;
            return false;
        }
    }

    if (!eof) {
        *error = "The file has no end record (truncated?)";
        return false;
    }
    if (top == 0) {
        *error = "The file contains no data";
        return false;
    }
    flash.resize(top);
    image.swap(flash);
    return true;
}

}  // namespace svs_hex
