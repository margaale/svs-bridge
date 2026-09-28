// Host unit tests for the RFC 2217 parser (rfc2217_proto.h): Telnet framing, option negotiation and the
// COM-PORT replies pyserial expects. Built and run by tests/run.sh.

#include "rfc2217_proto.h"

#include <cstdio>
#include <cstring>
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
};

static Result feed(rfc2217_t &s, const Bytes &in, uint8_t modem = 0xb0) {
    uint8_t data[256], reply[256];
    rfc2217_io_t io = {};
    io.data = data;
    io.data_max = sizeof(data);
    io.reply = reply;
    io.reply_max = sizeof(reply);
    rfc2217_input(&s, in.data(), in.size(), &io, modem);
    return {Bytes(data, data + io.data_len), Bytes(reply, reply + io.reply_len)};
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

int main() {
    test_greeting();
    test_data_and_escape();
    test_data_split_across_reads();
    test_settings_are_acknowledged();
    test_notify_and_signature();
    test_bad_input();
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
