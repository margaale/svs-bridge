// RFC 2217 (Telnet COM Port Control) server side, without sockets: splits what a client sends into
// serial data and Telnet commands, and builds the replies. rfc2217.cpp runs it on TCP port 2217; the
// host unit tests (tests/test_rfc2217.cpp) drive it directly.
//
// Settings the client asks for (baud rate, data bits, parity, stop bits, control lines) are
// acknowledged with the value asked for, as clients such as pyserial expect, but change nothing: the
// SVS's line stays at 9600 8N1.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RFC2217_SB_MAX 64

typedef struct {
    int st;                        // parser state
    uint8_t verb;                  // WILL/WONT/DO/DONT being read
    uint8_t sb[RFC2217_SB_MAX];    // subnegotiation being read
    size_t sb_len;
    bool sb_overflow;
    uint32_t options_we;           // bit per option (< 32): we WILL
    uint32_t options_they;         // they WILL (we said DO)
    uint32_t baud;                 // last values asked for (reported only)
    uint8_t datasize, parity, stopsize;
    bool dtr;                      // DTR as the client last set it (off until it says so)
} rfc2217_t;

typedef struct {
    uint8_t *data;       // serial data for the SVS
    size_t data_len, data_max;
    uint8_t *reply;      // bytes to send back to the client
    size_t reply_len, reply_max;
    bool dtr_raised;     // set when the client turned DTR on (it was off): opening a serial port does that
                         // and resets an Arduino-like board, which the SVS's utility relies on
} rfc2217_io_t;

void rfc2217_init(rfc2217_t *s);

// What the server sends first: its offers (WILL BINARY, DO BINARY, WILL SGA, DO SGA, DO COM-PORT).
size_t rfc2217_greeting(rfc2217_t *s, uint8_t *out, size_t max);

// Bytes from the client. modem_state: the line's modem status for NOTIFY-MODEMSTATE (RFC 2217
// bits: 0x10 CTS, 0x20 DSR, 0x40 RI, 0x80 CD; the FTDI's modem status byte uses the same bits).
void rfc2217_input(rfc2217_t *s, const uint8_t *in, size_t len, rfc2217_io_t *io, uint8_t modem_state);

// NOTIFY-MODEMSTATE on the server's initiative: clients such as pyserial don't ask, they expect one
// at connect and on every change.
size_t rfc2217_modemstate(uint8_t modem_state, uint8_t *out, size_t max);

// Serial data for the client, with 0xFF doubled. Returns bytes written to out; *used says how much of
// `in` fit.
size_t rfc2217_escape(const uint8_t *in, size_t len, uint8_t *out, size_t max, size_t *used);

// The SVS answers Y<n> / G<n> with two lines: the input number, then the value. A tool that reads one
// line per command (the official utility) takes the number for the value and every later answer is one
// line behind. After the client asks Y<n> / G<n> (rfc2217_echo_expect), the first line coming from the
// SVS that is only that number is dropped; everything else passes untouched, and the value line follows.
#define RFC2217_ECHO_HOLD_MAX 40

typedef struct {
    int input;                          // the input asked about; -1: not waiting for an echo
    uint8_t hold[RFC2217_ECHO_HOLD_MAX]; // the line being read while waiting
    size_t hold_len;
    uint32_t dropped;                   // echo lines dropped so far
} rfc2217_echo_t;

void rfc2217_echo_init(rfc2217_echo_t *e);
void rfc2217_echo_expect(rfc2217_echo_t *e, int input);
bool rfc2217_echo_pending(const rfc2217_echo_t *e);

// Filters bytes from the SVS into out (room for len + RFC2217_ECHO_HOLD_MAX); returns how many.
size_t rfc2217_echo_filter(rfc2217_echo_t *e, const uint8_t *in, size_t len, uint8_t *out, size_t max);

// Gives up waiting (the answer never came): what is held goes out, unfiltered.
size_t rfc2217_echo_release(rfc2217_echo_t *e, uint8_t *out, size_t max);

// A readable description of Telnet/RFC 2217 bytes, for the traffic log: "WILL BINARY, DO COM-PORT,
// SET-BAUDRATE 9600, text "AT"". Works on one chunk as it came (a sequence cut at the end is marked),
// understands both directions (a server reply is the client's command + 100). NUL-terminated, truncated
// with "..." if out is too small.
void rfc2217_describe(const uint8_t *in, size_t len, char *out, size_t max);

#ifdef __cplusplus
}
#endif
