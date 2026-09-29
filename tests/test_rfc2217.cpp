// Host unit tests for the RFC 2217 parser (rfc2217_proto.h): Telnet framing, option negotiation and the
// COM-PORT replies pyserial expects. Built and run by tests/run.sh.

#include "rfc2217_proto.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

using Bytes = std::vector<uint8_t>;

struct Result {
    Bytes data, reply;
    bool dtr_raised;
};

static Result feed(rfc2217_t &s, const Bytes &in, uint8_t modem = 0xb0) {
    uint8_t data[256], reply[256];
    rfc2217_io_t io = {};
    io.data = data;
    io.data_max = sizeof(data);
    io.reply = reply;
    io.reply_max = sizeof(reply);
    rfc2217_input(&s, in.data(), in.size(), &io, modem);
    return {Bytes(data, data + io.data_len), Bytes(reply, reply + io.reply_len), io.dtr_raised};
}

static void test_greeting() {
    rfc2217_t s;
    rfc2217_init(&s);
    uint8_t out[32];
    const size_t n = rfc2217_greeting(&s, out, sizeof(out));
    const Bytes want = {255, 251, 0, 255, 253, 0, 255, 251, 3, 255, 253, 3, 255, 253, 44};
    CHECK(Bytes(out, out + n) == want);
    // The client's matching answers must not be answered again (no negotiation loop).
    const Result r = feed(s, {255, 253, 0, 255, 251, 0, 255, 253, 3, 255, 251, 3, 255, 251, 44});
    CHECK(r.data.empty());
    CHECK((r.reply == Bytes{255, 253, 44}));  // only COM-PORT gets confirmed
}

static void test_data_and_escape() {
    rfc2217_t s;
    rfc2217_init(&s);
    const Result r = feed(s, {'S', 'V', 'S', 255, 255, 'x', 255, 241 /* NOP */, 'y'});
    CHECK((r.data == Bytes{'S', 'V', 'S', 255, 'x', 'y'}));
    CHECK(r.reply.empty());

    uint8_t out[8];
    size_t used;
    const uint8_t in[] = {'a', 255, 'b'};
    CHECK(rfc2217_escape(in, 3, out, sizeof(out), &used) == 4 && used == 3);
    CHECK((Bytes(out, out + 4) == Bytes{'a', 255, 255, 'b'}));
    CHECK(rfc2217_escape(in, 3, out, 2, &used) == 1 && used == 1);  // the doubled 0xFF does not fit: not cut
}

static void test_data_split_across_reads() {
    rfc2217_t s;
    rfc2217_init(&s);
    Result a = feed(s, {'a', 255});
    Result b = feed(s, {255, 'b'});
    CHECK((a.data == Bytes{'a'}));
    CHECK((b.data == Bytes{255, 'b'}));
}

static void test_settings_are_acknowledged() {
    rfc2217_t s;
    rfc2217_init(&s);
    // SET-BAUDRATE 115200 -> the same value back as command 101
    Result r = feed(s, {255, 250, 44, 1, 0, 1, 0xc2, 0, 255, 240});
    CHECK((r.reply == Bytes{255, 250, 44, 101, 0, 1, 0xc2, 0, 255, 240}));
    CHECK(s.baud == 115200);
    // Asking (value 0) reports the current one
    r = feed(s, {255, 250, 44, 1, 0, 0, 0, 0, 255, 240});
    CHECK((r.reply == Bytes{255, 250, 44, 101, 0, 1, 0xc2, 0, 255, 240}));
    // SET-DATASIZE 7
    r = feed(s, {255, 250, 44, 2, 7, 255, 240});
    CHECK((r.reply == Bytes{255, 250, 44, 102, 7, 255, 240}));
    // SET-CONTROL 8 (DTR on) is acknowledged as asked
    r = feed(s, {255, 250, 44, 5, 8, 255, 240});
    CHECK((r.reply == Bytes{255, 250, 44, 105, 8, 255, 240}));
    // ... and only DTR going on (it was off) is reported, so a client opening its port restarts the SVS once
    CHECK(r.dtr_raised);
    r = feed(s, {255, 250, 44, 5, 8, 255, 240});
    CHECK(!r.dtr_raised);
    r = feed(s, {255, 250, 44, 5, 9, 255, 240});  // DTR off
    CHECK(!r.dtr_raised && r.reply == (Bytes{255, 250, 44, 105, 9, 255, 240}));
    r = feed(s, {255, 250, 44, 5, 11, 255, 240});  // RTS on: not DTR
    CHECK(!r.dtr_raised);
    r = feed(s, {255, 250, 44, 5, 8, 255, 240});
    CHECK(r.dtr_raised);
}

static void test_notify_and_signature() {
    rfc2217_t s;
    rfc2217_init(&s);
    Result r = feed(s, {255, 250, 44, 7, 0, 255, 240}, 0xb0);
    CHECK((r.reply == Bytes{255, 250, 44, 107, 0xb0, 255, 240}));
    r = feed(s, {255, 250, 44, 0, 255, 240});
    CHECK(r.reply.size() > 6 && r.reply[3] == 100);

    uint8_t out[16];
    const size_t n = rfc2217_modemstate(0xff, out, sizeof(out));
    CHECK((Bytes(out, out + n) == Bytes{255, 250, 44, 107, 255, 255, 255, 240}));  // 0xFF doubled
}

static void test_bad_input() {
    rfc2217_t s;
    rfc2217_init(&s);
    // An oversized subnegotiation is dropped without overflowing, and parsing carries on after it
    Bytes big = {255, 250, 44, 0};
    big.insert(big.end(), 200, 'x');
    big.push_back(255);
    big.push_back(240);
    big.push_back('o');
    big.push_back('k');
    Result r = feed(s, big);
    CHECK(r.reply.empty());
    CHECK((r.data == Bytes{'o', 'k'}));
    // Unknown options are refused
    r = feed(s, {255, 251, 99, 255, 253, 99});
    CHECK((r.reply == Bytes{255, 254, 99, 255, 252, 99}));
}

static std::string describe(const Bytes &b, size_t max = 256) {
    char out[512];
    rfc2217_describe(b.data(), b.size(), out, max < sizeof(out) ? max : sizeof(out));
    return out;
}

static void test_describe() {
    // The log lines of a real client: what the bridge offers, what the client asks, what is answered.
    CHECK(describe({255, 251, 0, 255, 253, 0, 255, 251, 3, 255, 253, 3, 255, 253, 44}) ==
          "WILL BINARY, DO BINARY, WILL SUPPRESS-GO-AHEAD, DO SUPPRESS-GO-AHEAD, DO COM-PORT");
    CHECK(describe({255, 250, 44, 1, 0, 0, 0x25, 0x80, 255, 240, 255, 250, 44, 2, 8, 255, 240, 255, 250, 44, 3, 1, 255, 240}) ==
          "SET-BAUDRATE 9600, SET-DATASIZE 8, SET-PARITY none");
    CHECK(describe({255, 250, 44, 1, 0, 0, 0, 0, 255, 240}) == "SET-BAUDRATE query");
    CHECK(describe({255, 250, 44, 101, 0, 1, 0xC2, 0, 255, 240}) == "SET-BAUDRATE (reply) 115200");
    CHECK(describe({255, 250, 44, 5, 8, 255, 240}) == "SET-CONTROL DTR on");
    CHECK(describe({255, 250, 44, 7, 0xb0, 255, 240}) == "MODEMSTATE 0B0 (CTS DSR CD)" ||
          describe({255, 250, 44, 7, 0xb0, 255, 240}) == "MODEMSTATE B0 (CTS DSR CD)");
    // Data, with a doubled 0xFF and a control character.
    CHECK(describe({'A', 'T', 13, 255, 255}) == "text \"AT<0D>\", text \"<FF>\"");
    // A sequence cut by the end of the chunk is said so, not misread.
    CHECK(describe({255, 251}) == "(cut after IAC 251)");
    CHECK(describe({255, 250, 44, 1, 0}) == "(subnegotiation cut)");
    // Too small a buffer: truncated, terminated.
    const std::string small = describe({255, 251, 0, 255, 253, 0, 255, 251, 3}, 12);
    CHECK(small.size() == 11 && small.substr(8) == "...");
}

static std::string echo_filter(rfc2217_echo_t &e, const std::string &in) {
    uint8_t out[256];
    const size_t n = rfc2217_echo_filter(&e, (const uint8_t *)in.data(), in.size(), out, sizeof(out));
    return std::string((const char *)out, n);
}

static void test_echo_filter() {
    rfc2217_echo_t e;
    rfc2217_echo_init(&e);
    // Nothing asked: everything passes
    CHECK(echo_filter(e, "SVS TOTAL INPUTS=4\r\n1\r\n") == "SVS TOTAL INPUTS=4\r\n1\r\n");
    // Y1 -> "1", "1": the first is the input number
    rfc2217_echo_expect(&e, 1);
    CHECK(echo_filter(e, "1\r\n1\r\n") == "1\r\n");
    CHECK(!rfc2217_echo_pending(&e) && e.dropped == 1);
    // Y4 -> "4", "0", with a status line first and the answer split anywhere
    rfc2217_echo_expect(&e, 4);
    std::string got;
    for (char c : std::string("SVS TOTAL INPUTS=4\r\nSVS CURRENT INPUT=0\r\n4\r\n0\r\n")) got += echo_filter(e, std::string(1, c));
    CHECK(got == "SVS TOTAL INPUTS=4\r\nSVS CURRENT INPUT=0\r\n0\r\n");
    // Not the number asked about (a stale answer): passes, and the wait goes on
    rfc2217_echo_expect(&e, 2);
    CHECK(echo_filter(e, "7\r\n") == "7\r\n" && rfc2217_echo_pending(&e));
    CHECK(echo_filter(e, "2\r\n1\r\n") == "1\r\n");
    // The answer never comes whole: released as it is
    rfc2217_echo_expect(&e, 3);
    CHECK(echo_filter(e, "3") == "" && rfc2217_echo_pending(&e));
    uint8_t out[64];
    CHECK(rfc2217_echo_release(&e, out, sizeof(out)) == 1 && out[0] == '3' && !rfc2217_echo_pending(&e));
    // A line too long to be a number is not held for ever
    rfc2217_echo_expect(&e, 1);
    const std::string junk(60, 'x');
    CHECK(echo_filter(e, junk).size() >= 40 && !rfc2217_echo_pending(&e));
}

int main() {
    test_greeting();
    test_data_and_escape();
    test_data_split_across_reads();
    test_settings_are_acknowledged();
    test_notify_and_signature();
    test_bad_input();
    test_describe();
    test_echo_filter();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
