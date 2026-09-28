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
} rfc2217_t;

typedef struct {
    uint8_t *data;       // serial data for the SVS
    size_t data_len, data_max;
    uint8_t *reply;      // bytes to send back to the client
    size_t reply_len, reply_max;
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

#ifdef __cplusplus
}
#endif
