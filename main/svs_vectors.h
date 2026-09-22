// AVR vector-bootloader patch math for the SVS (ATmega328P + urboot).
//
// This is the delicate part of flashing: rewriting the reset vector so it
// jumps to the bootloader while preserving the application's real entry point.
// It mirrors avrdude's urclock programmer exactly, so the bytes the bridge
// writes are identical to what the official tool writes.
//
// Everything here is pure integer/opcode arithmetic with no ESP-IDF or hardware
// dependency, so it is unit-tested on the host (see test/host/test_svs_vectors).
// Keep it that way: no logging, no I/O, no globals.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace svs_vectors {

constexpr int VEC_SZ = 4;  // ATmega328P uses 4-byte jmp vectors

inline uint32_t buf2u32(const uint8_t *b)
{
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}

inline void u32tobuf(uint8_t *b, uint32_t v)
{
    b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}

inline bool is_rjmp(uint16_t op) { return (op & 0xF000) == 0xC000; }
inline bool is_jmp(uint16_t op) { return (op & 0xFE0E) == 0x940C; }

inline int rjmp_dist_wrap(int dist, int flashsize)
{
    int size = flashsize > 8182 ? 8192 : flashsize;
    if ((size & (size - 1)) == 0) {  // power of two
        dist &= size - 1;
        if (dist >= size / 2) {
            dist -= size;
        }
    }
    return dist;
}

// Byte address an rjmp at address 0 (the reset) jumps to
inline int rjmp_reset_target(uint16_t op, int flashsize)
{
    int16_t dist = op & 0x0FFF;
    dist = (int16_t)(dist << 4) >> 3;  // sign-extend 12 bits and multiply by 2
    int addr = rjmp_dist_wrap(dist + 2, flashsize);
    while (addr < 0) addr += flashsize;
    while (addr > flashsize) addr -= flashsize;
    return addr;
}

// Byte address a 4-byte jmp opcode targets
inline int jmp_target(const uint8_t *b)
{
    uint32_t op = buf2u32(b);
    int addr = op >> 16;
    addr |= (op & 1) << 16;
    addr |= (op & 0x1F0) << (17 - 4);
    return addr << 1;
}

// 4-byte jmp opcode to a byte address
inline uint32_t jmp_opcode(int32_t addr)
{
    return (((addr >> 1) & 0xFFFF) << 16) | 0x940C | (((addr >> 18) & 31) << 4) | ((addr >> 17) & 1);
}

// Reset rjmp that reaches the bootloader (urboot's own formula)
inline uint16_t rjmp_to_bootloader(int blstart, int flashsize)
{
    return 0xC000 | ((uint16_t)((blstart - flashsize - 2) / 2) & 0x0FFF);
}

// Patches a flash image so the reset vector points at the bootloader and the
// application's real entry is preserved in the bootloader's chosen vector,
// exactly as avrdude does for this urboot: it writes ONLY the 2-byte rjmp over
// the first word of the reset and leaves the second word (image[2..3]) as it
// came from the .hex — the low word of the original reset jmp, i.e. the app
// address. Confirmed against the SVS: a firmware whose reset is `jmp 0x1A6`
// (0C 94 D3 00) ends up on the chip as 7F CF D3 00. Returns false (and *error)
// without touching the image if the reset opcode is not a jmp/rjmp or the app
// entry is out of range. reset_rjmp is the 2 bytes written over the reset word.
inline bool patch_vectors(std::vector<uint8_t> &image, int blstart, int vector_num, int flashsize,
                          uint16_t &reset_rjmp, int &app_start, std::string *error)
{
    char m[96];
    int app_vec_loc = vector_num * VEC_SZ;
    if (image.size() < (size_t)app_vec_loc + VEC_SZ) {
        *error = "Image too small to patch";
        return false;
    }
    uint16_t reset_op16 = (uint16_t)(image[0] | image[1] << 8);
    if (is_jmp(reset_op16)) {
        app_start = jmp_target(&image[0]);
    } else if (is_rjmp(reset_op16)) {
        app_start = rjmp_reset_target(reset_op16, flashsize);
    } else {
        snprintf(m, sizeof(m), "Reset word 0x%04X is not a jmp or rjmp; not patching", reset_op16);
        *error = m;
        return false;
    }

    reset_rjmp = rjmp_to_bootloader(blstart, flashsize);
    if (app_start == blstart) {
        return true;  // already patched (a fresh .hex should not be)
    }
    if (app_start < app_vec_loc || app_start >= (int)image.size()) {
        snprintf(m, sizeof(m), "App start 0x%04X out of range [0x%04X, 0x%04X); not patching",
                 app_start, app_vec_loc, (unsigned)image.size());
        *error = m;
        return false;
    }

    image[0] = reset_rjmp & 0xFF;                          // reset word -> rjmp to bootloader
    image[1] = reset_rjmp >> 8;                            // image[2..3] left as-is (avrdude does too)
    u32tobuf(&image[app_vec_loc], jmp_opcode(app_start));  // moved-away vector -> app start

    // The patched reset must decode back to the bootloader, or do not proceed
    if (rjmp_reset_target(reset_rjmp, flashsize) != blstart) {
        *error = "Internal check failed: patched reset does not reach the bootloader";
        return false;
    }
    return true;
}

}  // namespace svs_vectors
