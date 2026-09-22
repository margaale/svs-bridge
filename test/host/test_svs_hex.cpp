// Host unit tests for the Intel HEX decoder (svs_hex.h). The decoder turns an
// untrusted .hex file into the bytes flashed to the SVS, so malformed input
// must be rejected rather than producing a bad image.
//
//   g++ -std=c++17 -Wall -Wextra -Imain -o test_svs_hex test/host/test_svs_hex.cpp
//   ./test_svs_hex

#include "svs_hex.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace svs_hex;

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static bool decode(const std::string &hex, std::vector<uint8_t> &image, std::string &err,
                   size_t max_size = 32768)
{
    return decode_hex(hex.data(), hex.size(), image, max_size, &err);
}

// A well-formed file: reset vector bytes at address 0, then the end record.
static void test_valid()
{
    std::vector<uint8_t> image;
    std::string err;
    bool ok = decode(":040000000C94D30089\n:00000001FF\n", image, err);
    CHECK(ok);
    CHECK(err.empty());
    CHECK(image.size() == 4);
    CHECK(image[0] == 0x0C && image[1] == 0x94 && image[2] == 0xD3 && image[3] == 0x00);
}

// An extended segment address (type 02) shifts where the next data lands.
static void test_segment_address()
{
    std::vector<uint8_t> image;
    std::string err;
    // base = 0x0010 << 4 = 0x100, then write 4 bytes there.
    bool ok = decode(":020000020010EC\n:0400000012345678E8\n:00000001FF\n", image, err);
    CHECK(ok);
    CHECK(image.size() == 0x104);
    CHECK(image[0] == 0xFF && image[0xFF] == 0xFF);  // gap stays erased
    CHECK(image[0x100] == 0x12 && image[0x101] == 0x34 &&
          image[0x102] == 0x56 && image[0x103] == 0x78);
}

static void test_bad_checksum()
{
    std::vector<uint8_t> image;
    std::string err;
    CHECK(!decode(":040000000C94D30000\n:00000001FF\n", image, err));
    CHECK(err.find("checksum") != std::string::npos);
}

static void test_not_a_record()
{
    std::vector<uint8_t> image;
    std::string err;
    CHECK(!decode("040000000C94D30089\n:00000001FF\n", image, err));  // missing ':'
}

static void test_truncated()
{
    std::vector<uint8_t> image;
    std::string err;
    CHECK(!decode(":040000000C94D30089\n", image, err));  // no end record
    CHECK(err.find("end record") != std::string::npos);
}

static void test_unsupported_type()
{
    std::vector<uint8_t> image;
    std::string err;
    CHECK(!decode(":000000000AF6\n:00000001FF\n", image, err));  // record type 0x0A
}

static void test_out_of_range()
{
    std::vector<uint8_t> image;
    std::string err;
    // 4 bytes at 0x7FFE run past the 32 KB flash.
    CHECK(!decode(":047FFE00000000007F\n:00000001FF\n", image, err));
    CHECK(err.find("past") != std::string::npos);
}

static void test_no_data()
{
    std::vector<uint8_t> image;
    std::string err;
    CHECK(!decode(":00000001FF\n", image, err));  // end record only
    CHECK(err.find("no data") != std::string::npos);
}

int main()
{
    test_valid();
    test_segment_address();
    test_bad_checksum();
    test_not_a_record();
    test_truncated();
    test_unsupported_type();
    test_out_of_range();
    test_no_data();

    if (g_failures == 0) {
        std::printf("All svs_hex tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
