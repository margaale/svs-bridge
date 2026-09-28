// The SVS's settings: where they live in its EEPROM and how to read and write
// them over the serial line.
//
// None of this is in the SVS's serial documentation. It is what the official
// SVS Management Utility does (SVS_Management_Utility_V1.41.ps1,
// tools/prioritydialogue.ps1 and tools/IReditor.ps1 in
// Arthrimus/SVS_Firmware_Repository), which reads and writes the control
// module's EEPROM byte by byte:
//
//   R<addr>          the SVS answers with the byte at addr, in decimal, on a
//                    line of its own (between its "SVS ..." status lines)
//   W<vvv><addr>     writes value vvv (3 decimal digits) at addr
//   Y<n> / G<n>      whether the RGB -> YPbPr (Y) or YPbPr -> RGB (G)
//                    transcoder is on for input n: 0 = on
//
// Per-input flags are bitmaps: input n (1-based) is bit (n-1)%8 of the byte at
// base + (n-1)/8. Some are active-low; each constant below says which.
//
// Pure: no ESP-IDF, no I/O, no globals. Tested in tests/test_svs_config.cpp.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace svs_config {

// The EEPROM map
constexpr int IR_ADDRESS_BASE = 0;    // IR codes: address byte of each slot (0..249)
constexpr int IR_COMMAND_OFFSET = 250;  // ... its command byte, 250 further on
constexpr int IR_REGION = 250;
constexpr uint8_t IR_EMPTY = 0xFF;    // an unused slot

constexpr int AUTO_PROFILE_BASE = 513;  // bitmap, 1 = send the auto profile / IR codes
constexpr int RGSB_BASE = 550;          // bitmap, 0 = convert RGBS to RGsB (V3 modules)
constexpr int SYNC_BYPASS_BASE = 590;   // bitmap, 1 = sync bypass (V3 SCART)
constexpr int SCART_V3_BASE = 630;      // bitmap, 0 = a V3 SCART module on that input
constexpr int ANY_SCART_V3 = 669;       // 1 = at least one V3 SCART module
constexpr int VGA_V3_BASE = 670;        // bitmap, 0 = a V3 VGA module on that input
constexpr int ANY_VGA_V3 = 709;         // 1 = at least one V3 VGA module
constexpr int TX_RGB_TO_YPBPR = 710;    // 0 = RGB -> YPbPr transcoder fitted
constexpr int TX_YPBPR_TO_RGB = 711;    // 0 = YPbPr -> RGB transcoder fitted

// The SVS takes up to 32 inputs; the bitmaps above leave room for that
constexpr int MAX_INPUTS = 32;

// The firmware that has all of the above (the EEPROM commands came in 1.16,
// the per-input sync and auto profile settings in 1.20)
constexpr int MIN_FIRMWARE = 120;

// "SVS_FW_1.21" -> 121, "SVS_FW_1.20_BETA" -> 120; -1 if it is not a banner
inline int firmware_number(const std::string &banner)
{
    const std::string prefix = "SVS_FW_";
    if (banner.rfind(prefix, 0) != 0) {
        return -1;
    }
    int major = 0, minor = 0;
    char dot = 0;
    if (std::sscanf(banner.c_str() + prefix.size(), "%d%c%d", &major, &dot, &minor) != 3 || dot != '.' ||
        major < 0 || minor < 0 || minor > 99) {
        return -1;
    }
    return major * 100 + minor;
}

// --- Bitmaps -------------------------------------------------------------------

struct Bit {
    int addr;
    uint8_t mask;
};

inline Bit bit_of(int base, int input)  // input is 1-based
{
    return {base + (input - 1) / 8, (uint8_t)(1u << ((input - 1) % 8))};
}

// Bytes a bitmap of `inputs` inputs occupies
inline int bitmap_bytes(int inputs) { return (inputs + 7) / 8; }

inline uint8_t with_bit(uint8_t byte, uint8_t mask, bool set)
{
    return set ? (uint8_t)(byte | mask) : (uint8_t)(byte & ~mask);
}

// --- IR codes ------------------------------------------------------------------
//
// The 250 address bytes are shared by all inputs: slot k (0-based) of input n
// is at n + k * (inputs + 1), and its command byte 250 further on. So the
// fewer inputs, the more codes each one can hold.

inline int ir_slots(int inputs) { return inputs > 0 ? IR_REGION / (inputs + 1) : 0; }

inline int ir_address_addr(int input, int slot, int inputs) { return IR_ADDRESS_BASE + input + slot * (inputs + 1); }

inline int ir_command_addr(int input, int slot, int inputs)
{
    return ir_address_addr(input, slot, inputs) + IR_COMMAND_OFFSET;
}

struct IrCode {
    uint8_t address;
    uint8_t command;
    bool operator==(const IrCode &o) const { return address == o.address && command == o.command; }
};

// --- Serial commands -------------------------------------------------------------

inline std::string read_command(int addr) { return "R" + std::to_string(addr); }

// The utility always sends the value as 3 digits and the IR editor the address
// too; the SVS takes the first 3 digits as the value and the rest as the address
inline std::string write_command(int addr, uint8_t value)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "W%03u%03d", (unsigned)value, addr);
    return buf;
}

// The SVS's answer to R/Y/G: the first line that is not one of its "SVS ..."
// status lines. Like the utility, every digit on it makes the number.
// -1 if this line is not an answer (a status line, no digits, not a byte).
inline int parse_byte_reply(const std::string &line)
{
    if (line.rfind("SVS", 0) == 0) {
        return -1;
    }
    int value = 0;
    bool any = false;
    for (char c : line) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            any = true;
            if (value > 255) {
                return -1;
            }
        }
    }
    return any ? value : -1;
}

// --- Settings ------------------------------------------------------------------

struct InputSettings {
    bool auto_profile = true;
    bool rgsb = false;          // RGBS -> RGsB, V3 SCART and V3 VGA only
    bool sync_bypass = false;   // V3 SCART only
    bool rgb_to_ypbpr = false;  // per-input transcoder use (not in the EEPROM map: Y<n>)
    bool ypbpr_to_rgb = false;  // (G<n>)
    std::vector<IrCode> ir;
};

// What the SVS reports about its hardware, read along with the settings
struct Hardware {
    bool tx_rgb_to_ypbpr = false;  // transcoders fitted
    bool tx_ypbpr_to_rgb = false;
    std::vector<bool> scart_v3;    // per input (index 0 = input 1)
    std::vector<bool> vga_v3;
};

// A byte-addressed view of the EEPROM bytes the bridge has read
struct Eeprom {
    std::vector<int> bytes = std::vector<int>(1024, -1);  // -1 = not read

    int get(int addr) const { return addr >= 0 && addr < (int)bytes.size() ? bytes[addr] : -1; }
    void set(int addr, uint8_t v)
    {
        if (addr >= 0 && addr < (int)bytes.size()) {
            bytes[addr] = v;
        }
    }
    bool bit(int base, int input, bool active_low) const
    {
        Bit b = bit_of(base, input);
        int v = get(b.addr);
        bool set = v >= 0 && (v & b.mask);
        return active_low ? !set : set;
    }
};

// The addresses to read, besides the IR slots (read until the first empty one)
inline std::vector<int> bitmap_addresses(int inputs)
{
    std::vector<int> out;
    for (int base : {AUTO_PROFILE_BASE, RGSB_BASE, SYNC_BYPASS_BASE, SCART_V3_BASE, VGA_V3_BASE}) {
        for (int i = 0; i < bitmap_bytes(inputs); i++) {
            out.push_back(base + i);
        }
    }
    out.push_back(TX_RGB_TO_YPBPR);
    out.push_back(TX_YPBPR_TO_RGB);
    return out;
}

inline Hardware decode_hardware(const Eeprom &e, int inputs)
{
    Hardware hw;
    hw.tx_rgb_to_ypbpr = e.get(TX_RGB_TO_YPBPR) == 0;
    hw.tx_ypbpr_to_rgb = e.get(TX_YPBPR_TO_RGB) == 0;
    for (int n = 1; n <= inputs; n++) {
        hw.scart_v3.push_back(e.bit(SCART_V3_BASE, n, true));
        hw.vga_v3.push_back(e.bit(VGA_V3_BASE, n, true));
    }
    return hw;
}

// The EEPROM part of an input's settings (the transcoders are read with Y/G)
inline void decode_input(const Eeprom &e, int input, int inputs, InputSettings &s)
{
    s.auto_profile = e.bit(AUTO_PROFILE_BASE, input, false);
    s.rgsb = e.bit(RGSB_BASE, input, true);
    s.sync_bypass = e.bit(SYNC_BYPASS_BASE, input, false);
    s.ir.clear();
    for (int k = 0; k < ir_slots(inputs); k++) {
        int a = e.get(ir_address_addr(input, k, inputs));
        int c = e.get(ir_command_addr(input, k, inputs));
        if (a < 0 || a == IR_EMPTY || c < 0) {
            break;
        }
        s.ir.push_back({(uint8_t)a, (uint8_t)c});
    }
}

// The bytes to write so the EEPROM holds `want`, given what was read. Only
// bytes that change are written, and bits of inputs the bridge does not
// manage (past `inputs`) keep what was read. Returns false (and writes
// nothing) if a byte it needs was not read or an input has more IR codes than
// fit; *error says why.
inline bool plan_writes(const Eeprom &read, const std::vector<InputSettings> &want, int inputs,
                        std::vector<std::pair<int, uint8_t>> &writes, std::string *error)
{
    writes.clear();
    if ((int)want.size() != inputs || inputs < 1 || inputs > MAX_INPUTS) {
        if (error) *error = "The settings do not match the SVS's " + std::to_string(inputs) + " inputs";
        return false;
    }
    Eeprom out = read;
    auto put_bit = [&](int base, int input, bool on, bool active_low) {
        Bit b = bit_of(base, input);
        int v = out.get(b.addr);
        if (v < 0) {
            return false;
        }
        out.set(b.addr, with_bit((uint8_t)v, b.mask, active_low ? !on : on));
        return true;
    };
    const int slots = ir_slots(inputs);
    for (int n = 1; n <= inputs; n++) {
        const InputSettings &s = want[n - 1];
        if (!put_bit(AUTO_PROFILE_BASE, n, s.auto_profile, false) || !put_bit(RGSB_BASE, n, s.rgsb, true) ||
            !put_bit(SYNC_BYPASS_BASE, n, s.sync_bypass, false)) {
            if (error) *error = "Read the settings from the SVS first";
            return false;
        }
        if ((int)s.ir.size() > slots) {
            if (error) {
                *error = "Input " + std::to_string(n) + " has " + std::to_string(s.ir.size()) +
                         " IR codes; with " + std::to_string(inputs) + " inputs each one holds " +
                         std::to_string(slots);
            }
            return false;
        }
        // The codes, then one empty slot to end the list (if the old list was
        // longer, its tail is cleared too so nothing stale can come back)
        InputSettings old;
        decode_input(read, n, inputs, old);
        size_t end = std::max(s.ir.size() + 1, old.ir.size() + 1);
        for (size_t k = 0; k < end && (int)k < slots; k++) {
            bool used = k < s.ir.size();
            out.set(ir_address_addr(n, (int)k, inputs), used ? s.ir[k].address : IR_EMPTY);
            out.set(ir_command_addr(n, (int)k, inputs), used ? s.ir[k].command : IR_EMPTY);
        }
    }
    for (size_t a = 0; a < out.bytes.size(); a++) {
        if (out.bytes[a] >= 0 && out.bytes[a] != read.bytes[a]) {
            writes.push_back({(int)a, (uint8_t)out.bytes[a]});
        }
    }
    return true;
}

}  // namespace svs_config
