// Host unit tests for parsing the SVS's serial output (svs_protocol.h).
// Detecting the active-input change is the core of the Home Assistant feature,
// so the classification and de-duplication are pinned here.
//
//   g++ -std=c++17 -Wall -Wextra -Imain -o test_svs_protocol test/host/test_svs_protocol.cpp
//   ./test_svs_protocol

#include "svs_protocol.h"

#include <cstdio>
#include <string>

using namespace svs_protocol;

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static void test_trailing_number()
{
    CHECK(trailing_number("SVS NEW INPUT=4") == 4);
    CHECK(trailing_number("SVS CURRENT INPUT=12") == 12);
    CHECK(trailing_number("SVS TOTAL INPUTS=0") == 0);
    CHECK(trailing_number("no digits here") == -1);
    CHECK(trailing_number("") == -1);
}

static void test_classify()
{
    CHECK(parse_svs_line("SVS_FW_1.20").kind == LineKind::Firmware);

    ParsedLine changed = parse_svs_line("SVS NEW INPUT=4");
    CHECK(changed.kind == LineKind::InputChange);
    CHECK(changed.value == 4);

    ParsedLine current = parse_svs_line("SVS CURRENT INPUT=3");
    CHECK(current.kind == LineKind::InputChange);
    CHECK(current.value == 3);

    ParsedLine zero = parse_svs_line("SVS CURRENT INPUT=0");  // 0 = no active input
    CHECK(zero.kind == LineKind::InputChange);
    CHECK(zero.value == 0);

    ParsedLine total = parse_svs_line("SVS TOTAL INPUTS=8");
    CHECK(total.kind == LineKind::TotalInputs);
    CHECK(total.value == 8);

    CHECK(parse_svs_line("something else").kind == LineKind::None);
}

static void test_only_control_chars()
{
    CHECK(only_control_chars("<1A>"));
    CHECK(only_control_chars("<1A><0D>"));
    CHECK(!only_control_chars("SVS CURRENT INPUT=3"));
    CHECK(!only_control_chars("<1A>x"));
}

// The heartbeat repeats every ~2 s; only value changes should get through.
static void test_status_filter()
{
    StatusFilter f;
    CHECK(!f.is_repeated("SVS CURRENT INPUT=3"));  // first time: keep
    CHECK(f.is_repeated("SVS CURRENT INPUT=3"));   // same: drop
    CHECK(!f.is_repeated("SVS CURRENT INPUT=4"));  // changed: keep
    CHECK(!f.is_repeated("SVS TOTAL INPUTS=8"));   // different field: keep
    CHECK(f.is_repeated("SVS TOTAL INPUTS=8"));    // same: drop

    // A reboot banner resets the memory, so the next status is kept again.
    CHECK(!f.is_repeated("SVS_FW_1.20"));
    CHECK(!f.is_repeated("SVS CURRENT INPUT=4"));

    // Input-change lines are never de-duplicated (each one is an event).
    CHECK(!f.is_repeated("SVS NEW INPUT=4"));
    CHECK(!f.is_repeated("SVS NEW INPUT=4"));
}

int main()
{
    test_trailing_number();
    test_classify();
    test_only_control_chars();
    test_status_filter();

    if (g_failures == 0) {
        std::printf("All svs_protocol tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
