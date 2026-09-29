// Host unit tests for the SVS settings map (svs_config.h). A wrong address or
// polarity here would silently change settings on every input of a real SVS,
// so the map, the commands and the write plan are pinned here.
//
//   g++ -std=c++17 -Wall -Wextra -Isrc/core -o test_svs_config tests/test_svs_config.cpp
//   ./test_svs_config   (or tests/run.sh for all of them)

#include "svs_config.h"

#include <cstdio>
#include <string>

using namespace svs_config;

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static void test_firmware_number()
{
    CHECK(firmware_number("SVS_FW_1.21") == 121);
    CHECK(firmware_number("SVS_FW_1.20_BETA") == 120);
    CHECK(firmware_number("SVS_FW_1.16") == 116);
    CHECK(firmware_number("SVS CURRENT INPUT=3") == -1);
    CHECK(firmware_number("SVS_FW_") == -1);
}

static void test_bits()
{
    // Input 1 is bit 0 of the base byte, input 9 bit 0 of the next one
    CHECK(bit_of(513, 1).addr == 513 && bit_of(513, 1).mask == 0x01);
    CHECK(bit_of(513, 8).addr == 513 && bit_of(513, 8).mask == 0x80);
    CHECK(bit_of(513, 9).addr == 514 && bit_of(513, 9).mask == 0x01);
    CHECK(bitmap_bytes(8) == 1 && bitmap_bytes(9) == 2 && bitmap_bytes(32) == 4);
    CHECK(with_bit(0x00, 0x04, true) == 0x04);
    CHECK(with_bit(0xFF, 0x04, false) == 0xFB);
}

static void test_ir_layout()
{
    // As the utility: floor(250 / (inputs + 1)) slots, interleaved
    CHECK(ir_slots(8) == 27);
    CHECK(ir_slots(32) == 7);
    CHECK(ir_address_addr(1, 0, 8) == 1);
    CHECK(ir_address_addr(1, 1, 8) == 10);
    CHECK(ir_address_addr(8, 26, 8) == 8 + 26 * 9);
    CHECK(ir_address_addr(8, 26, 8) < IR_REGION);
    CHECK(ir_command_addr(3, 2, 8) == 3 + 2 * 9 + 250);
    // The last slot of the last input always fits
    for (int n = 1; n <= MAX_INPUTS; n++) {
        CHECK(ir_address_addr(n, ir_slots(n) - 1, n) < IR_REGION);
    }
}

static void test_commands()
{
    CHECK(read_command(590) == "R590");
    CHECK(write_command(513, 5) == "W005513");
    CHECK(write_command(513, 255) == "W255513");
    CHECK(write_command(1, 0x49) == "W073001");  // IR slots: address padded too
}

static void test_reply()
{
    CHECK(parse_byte_reply("255") == 255);
    CHECK(parse_byte_reply(" 0\r") == 0);
    CHECK(parse_byte_reply("SVS CURRENT INPUT=3") == -1);
    CHECK(parse_byte_reply("") == -1);
    CHECK(parse_byte_reply("1234") == -1);

    // Y<n>/G<n>: the input number, then the value (from a real SVS: only input 4 had it on)
    CHECK(transcoder_value({"1", "1"}) == 1);
    CHECK(transcoder_value({"4", "0"}) == 0);
    CHECK(transcoder_value({"4\r", " 0\r"}) == 0);
    CHECK(transcoder_value({"3"}) == 3);  // a single line is taken as the value
    CHECK(transcoder_value({"SVS CURRENT INPUT=0", "2", "1"}) == 1);
    CHECK(transcoder_value({"br_9600"}) == -1);
    CHECK(transcoder_value({}) == -1);
}

// An 8-input SVS: V3 SCART on input 3, V3 VGA on 6, RGB -> YPbPr fitted
static Eeprom sample()
{
    Eeprom e;
    for (int a : bitmap_addresses(8)) e.set(a, 0xFF);
    e.set(AUTO_PROFILE_BASE, 0x7F);      // on for 1..7, off for 8
    e.set(RGSB_BASE, 0xFF & ~0x04);      // active-low: on for input 3
    e.set(SYNC_BYPASS_BASE, 0x00);
    e.set(SCART_V3_BASE, 0xFF & ~0x04);  // active-low: V3 SCART on 3
    e.set(VGA_V3_BASE, 0xFF & ~0x20);    // V3 VGA on 6
    e.set(TX_RGB_TO_YPBPR, 0);           // fitted
    e.set(TX_YPBPR_TO_RGB, 1);           // not fitted
    // Input 3: one code (Profile 3 on a RetroTINK 4K), then the end
    e.set(ir_address_addr(3, 0, 8), 0x49);
    e.set(ir_command_addr(3, 0, 8), 0x03);
    e.set(ir_address_addr(3, 1, 8), IR_EMPTY);
    e.set(ir_command_addr(3, 1, 8), IR_EMPTY);
    for (int n = 1; n <= 8; n++) {
        if (n != 3) {
            e.set(ir_address_addr(n, 0, 8), IR_EMPTY);
            e.set(ir_command_addr(n, 0, 8), IR_EMPTY);
        }
    }
    return e;
}

static void test_decode()
{
    Eeprom e = sample();
    Hardware hw = decode_hardware(e, 8);
    CHECK(hw.tx_rgb_to_ypbpr);
    CHECK(!hw.tx_ypbpr_to_rgb);
    CHECK(hw.scart_v3.size() == 8 && hw.scart_v3[2] && !hw.scart_v3[0]);
    CHECK(hw.vga_v3[5] && !hw.vga_v3[2]);

    InputSettings s3, s8;
    decode_input(e, 3, 8, s3);
    decode_input(e, 8, 8, s8);
    CHECK(s3.auto_profile && s3.rgsb && !s3.sync_bypass);
    CHECK(s3.ir.size() == 1 && s3.ir[0].address == 0x49 && s3.ir[0].command == 0x03);
    CHECK(!s8.auto_profile && !s8.rgsb && s8.ir.empty());
}

static std::vector<InputSettings> decode_all(const Eeprom &e, int inputs)
{
    std::vector<InputSettings> v(inputs);
    for (int n = 1; n <= inputs; n++) decode_input(e, n, inputs, v[n - 1]);
    return v;
}

static void test_plan_nothing_changed()
{
    Eeprom e = sample();
    std::vector<std::pair<int, uint8_t>> w;
    CHECK(plan_writes(e, decode_all(e, 8), 8, w, nullptr));
    // Only the terminators of lists that were already empty may be rewritten
    for (auto &p : w) CHECK(p.second == IR_EMPTY);
}

static void test_plan_one_bit()
{
    Eeprom e = sample();
    auto want = decode_all(e, 8);
    want[2].sync_bypass = true;  // input 3
    std::vector<std::pair<int, uint8_t>> w;
    CHECK(plan_writes(e, want, 8, w, nullptr));
    bool found = false;
    for (auto &p : w) {
        if (p.first == SYNC_BYPASS_BASE) {
            found = true;
            CHECK(p.second == 0x04);
        }
        CHECK(p.first != AUTO_PROFILE_BASE && p.first != RGSB_BASE);  // untouched
    }
    CHECK(found);
}

static void test_plan_active_low_and_foreign_bits()
{
    // 5 inputs: bits 5..7 of each bitmap byte belong to no input and must keep
    // what was read
    Eeprom e;
    for (int a : bitmap_addresses(5)) e.set(a, 0xE0);
    for (int n = 1; n <= 5; n++) e.set(ir_address_addr(n, 0, 5), IR_EMPTY), e.set(ir_command_addr(n, 0, 5), IR_EMPTY);
    auto want = decode_all(e, 5);
    CHECK(want[0].rgsb);           // active-low: a clear bit is on
    want[0].rgsb = false;          // so turning it off sets bit 0
    want[1].auto_profile = true;   // active-high: sets bit 1
    std::vector<std::pair<int, uint8_t>> w;
    CHECK(plan_writes(e, want, 5, w, nullptr));
    for (auto &p : w) {
        if (p.first == RGSB_BASE) CHECK(p.second == 0xE1);
        if (p.first == AUTO_PROFILE_BASE) CHECK(p.second == 0xE2);
    }
}

static void test_plan_ir()
{
    Eeprom e = sample();
    auto want = decode_all(e, 8);
    want[0].ir = {{0x49, 0x0B}, {0x49, 0x24}};  // input 1: two codes
    want[2].ir.clear();                          // input 3: none
    std::vector<std::pair<int, uint8_t>> w;
    CHECK(plan_writes(e, want, 8, w, nullptr));
    Eeprom after = e;
    for (auto &p : w) after.set(p.first, p.second);
    auto got = decode_all(after, 8);
    CHECK(got[0].ir.size() == 2 && got[0].ir[1] == (IrCode{0x49, 0x24}));
    CHECK(got[2].ir.empty());
    CHECK(after.get(ir_address_addr(1, 2, 8)) == IR_EMPTY);  // list ended
}

static void test_plan_refuses()
{
    Eeprom e = sample();
    std::vector<std::pair<int, uint8_t>> w;
    std::string error;
    auto want = decode_all(e, 8);
    want[0].ir.assign(28, {1, 2});  // 27 fit with 8 inputs
    CHECK(!plan_writes(e, want, 8, w, &error));
    CHECK(w.empty() && !error.empty());

    Eeprom unread;  // nothing read: refuse rather than guess the other bits
    CHECK(!plan_writes(unread, decode_all(e, 8), 8, w, &error));
    CHECK(!plan_writes(e, decode_all(e, 7), 8, w, &error));  // wrong input count
}

int main()
{
    test_firmware_number();
    test_bits();
    test_ir_layout();
    test_commands();
    test_reply();
    test_decode();
    test_plan_nothing_changed();
    test_plan_one_bit();
    test_plan_active_low_and_foreign_bits();
    test_plan_ir();
    test_plan_refuses();
    if (g_failures == 0) {
        std::printf("test_svs_config: all tests passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
