// Host unit tests for the AVR vector-patch math (svs_vectors.h).
//
// This is the code that decides what bytes get written over an SVS's reset
// vector, so a regression here could brick the device. It runs on the host with
// no ESP-IDF, mirroring the independent Python check we validated against a real
// SVS. Build and run (from the repo root):
//
//   g++ -std=c++17 -Wall -Wextra -Imain -o test_svs_vectors test/host/test_svs_vectors.cpp
//   ./test_svs_vectors

#include "svs_vectors.h"

#include <cstdio>
#include <vector>

using namespace svs_vectors;

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

// jmp_opcode and jmp_target must be exact inverses for every reachable word
// address in the 32 KB flash; the app entry we preserve depends on it.
static void test_jmp_roundtrip()
{
    for (int addr = 0; addr < 0x8000; addr += 2) {
        uint8_t b[4];
        u32tobuf(b, jmp_opcode(addr));
        CHECK(is_jmp((uint16_t)(b[0] | b[1] << 8)));
        CHECK(jmp_target(b) == addr);
    }
}

// The reset rjmp the bridge computes must reach the bootloader and decode back
// to it. 0xCF7F is what avrdude wrote to the real SVS (blstart 0x7F00).
static void test_reset_rjmp()
{
    CHECK(rjmp_to_bootloader(0x7F00, 32768) == 0xCF7F);
    CHECK(rjmp_reset_target(0xCF7F, 32768) == 0x7F00);
    // A 2-byte rjmp reset only reaches the bootloader when it sits in the top
    // ~4 KB (the rjmp distance wraps modulo 8192 on this flash). urboot on the
    // ATmega328P lives there, so every 128-byte boundary in 0x7000..0x8000
    // must round-trip.
    for (int bl = 0x7000; bl < 0x8000; bl += 128) {
        uint16_t op = rjmp_to_bootloader(bl, 32768);
        CHECK(is_rjmp(op));
        CHECK(rjmp_reset_target(op, 32768) == bl);
    }
}

// The exact case confirmed on hardware: a firmware whose reset is `jmp 0x1A6`
// (0C 94 D3 00) with the app entry saved in vector 25 must end up on the chip
// with reset bytes 7F CF D3 00 (rjmp to 0x7F00, original app-address word kept).
static void test_known_svs_case()
{
    std::vector<uint8_t> image(32768, 0xFF);
    image[0] = 0x0C; image[1] = 0x94; image[2] = 0xD3; image[3] = 0x00;  // jmp 0x1A6

    uint16_t reset_rjmp = 0;
    int app_start = -1;
    std::string err;
    bool ok = patch_vectors(image, 0x7F00, 25, 32768, reset_rjmp, app_start, &err);

    CHECK(ok);
    CHECK(err.empty());
    CHECK(app_start == 0x1A6);
    CHECK(reset_rjmp == 0xCF7F);
    // Reset word replaced by the rjmp; the second word (app address) left as-is.
    CHECK(image[0] == 0x7F);
    CHECK(image[1] == 0xCF);
    CHECK(image[2] == 0xD3);
    CHECK(image[3] == 0x00);
    // The moved-away vector now holds a jmp to the real app entry.
    CHECK(jmp_target(&image[25 * VEC_SZ]) == 0x1A6);
}

// A reset that is neither jmp nor rjmp must be rejected without touching bytes.
static void test_rejects_unknown_reset()
{
    std::vector<uint8_t> image(32768, 0xFF);
    image[0] = 0x00; image[1] = 0x00; image[2] = 0x00; image[3] = 0x00;  // nop, not a jump

    uint16_t reset_rjmp = 0;
    int app_start = -1;
    std::string err;
    bool ok = patch_vectors(image, 0x7F00, 25, 32768, reset_rjmp, app_start, &err);

    CHECK(!ok);
    CHECK(!err.empty());
    CHECK(image[0] == 0x00 && image[1] == 0x00);  // untouched
}

// An rjmp reset (used by smaller AVRs) is decoded through the rjmp path.
static void test_rjmp_reset()
{
    std::vector<uint8_t> image(32768, 0xFF);
    // rjmp forward to 0x100: distance word = (0x100 - 2)/2 = 0x7F -> 0xC07F
    image[0] = 0x7F; image[1] = 0xC0;

    uint16_t reset_rjmp = 0;
    int app_start = -1;
    std::string err;
    bool ok = patch_vectors(image, 0x7F00, 25, 32768, reset_rjmp, app_start, &err);

    CHECK(ok);
    CHECK(app_start == 0x100);
    CHECK(image[0] == 0x7F && image[1] == 0xCF);  // now points at the bootloader
}

int main()
{
    test_jmp_roundtrip();
    test_reset_rjmp();
    test_known_svs_case();
    test_rejects_unknown_reset();
    test_rjmp_reset();

    if (g_failures == 0) {
        std::printf("All svs_vectors tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
