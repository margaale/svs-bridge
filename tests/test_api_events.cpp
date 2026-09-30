// Host unit tests for /api/v1/events' types (api_events.h): what ?types= asks for, and the names
// "hello" lists. Adding a type must never change what an existing client gets.
//
//   g++ -std=c++17 -Wall -Wextra -Isrc/core -o test_api_events tests/test_api_events.cpp
//   ./test_api_events   (or tests/run.sh for all of them)

#include "api_events.h"

#include <cstdio>
#include <string>

using namespace api_events;

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

static void test_parse()
{
    CHECK(parse("state") == STATE);
    CHECK(parse("state,later") == STATE);  // a type this bridge doesn't have: left out
    CHECK(parse(" later , state ") == STATE);
    CHECK(parse("state,state") == STATE);
    CHECK(parse("later") == 0);
    CHECK(parse("") == 0);
    CHECK(parse(",, ,") == 0);
    CHECK(parse("stat") == 0);
    CHECK(parse("states") == 0);
    CHECK(parse("State") == 0);  // names are lowercase, exactly
}

static void test_url_decode()
{
    CHECK(url_decode("state") == "state");
    CHECK(url_decode("state%2Clater") == "state,later");
    CHECK(url_decode("state%2clater") == "state,later");
    CHECK(url_decode("a+b") == "a b");
    CHECK(url_decode("100%") == "100%");    // a lone % stays
    CHECK(url_decode("%2") == "%2");        // cut short: stays
    CHECK(url_decode("%zz") == "%zz");      // not hex: stays
    CHECK(parse(url_decode("later%2Cstate")) == STATE);
}

static void test_names_json()
{
    CHECK(names_json(STATE) == "[\"state\"]");
    CHECK(names_json(0) == "[]");
    CHECK(names_json(0x80000000u | STATE) == "[\"state\"]");  // bits without a name: left out
    CHECK(names_json(ALL).size() > 2);
}

int main()
{
    test_parse();
    test_url_decode();
    test_names_json();

    if (g_failures == 0) {
        std::printf("All api_events tests passed\n");
        return 0;
    }
    std::printf("%d check(s) failed\n", g_failures);
    return 1;
}
