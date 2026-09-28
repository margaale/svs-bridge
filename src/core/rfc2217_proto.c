#include "rfc2217_proto.h"

#include <string.h>

#define IAC  255
#define DONT 254
#define DO   253
#define WONT 252
#define WILL 251
#define SB   250
#define SE   240

#define OPT_BINARY   0
#define OPT_SGA      3
#define OPT_COM_PORT 44

// COM-PORT-OPTION commands, client to server; the server answers with the same number + 100.
#define CPO_SIGNATURE          0
#define CPO_SET_BAUDRATE       1
#define CPO_SET_DATASIZE       2
#define CPO_SET_PARITY         3
#define CPO_SET_STOPSIZE       4
#define CPO_SET_CONTROL        5
#define CPO_NOTIFY_LINESTATE   6
#define CPO_NOTIFY_MODEMSTATE  7
#define CPO_FLOWCONTROL_SUSPEND 8
#define CPO_FLOWCONTROL_RESUME 9
#define CPO_SET_LINESTATE_MASK 10
#define CPO_SET_MODEMSTATE_MASK 11
#define CPO_PURGE_DATA         12
#define SERVER_OFFSET          100

#define SIGNATURE "SVS Bridge"

enum { ST_DATA, ST_IAC, ST_VERB, ST_SB, ST_SB_IAC };

// Options we accept, both ways: binary, suppress go-ahead, and (client side only) COM-PORT.
static bool we_accept(uint8_t opt) {
    return opt == OPT_BINARY || opt == OPT_SGA;
}

static bool they_accept(uint8_t opt) {
    return opt == OPT_BINARY || opt == OPT_SGA || opt == OPT_COM_PORT;
}

static uint32_t bit(uint8_t opt) {
    return opt < 32 ? 1u << opt : 0; // COM-PORT (44) is tracked separately
}

static void put(rfc2217_io_t *io, uint8_t b) {
    if (io->reply_len < io->reply_max) io->reply[io->reply_len++] = b;
}

static void put_cmd(rfc2217_io_t *io, uint8_t verb, uint8_t opt) {
    put(io, IAC);
    put(io, verb);
    put(io, opt);
}

// A COM-PORT reply: IAC SB 44 <cmd+100> <payload, 0xFF doubled> IAC SE.
static void put_cpo(rfc2217_io_t *io, uint8_t cmd, const uint8_t *payload, size_t len) {
    put(io, IAC);
    put(io, SB);
    put(io, OPT_COM_PORT);
    put(io, (uint8_t)(cmd + SERVER_OFFSET));
    for (size_t i = 0; i < len; i++) {
        put(io, payload[i]);
        if (payload[i] == IAC) put(io, IAC);
    }
    put(io, IAC);
    put(io, SE);
}

void rfc2217_init(rfc2217_t *s) {
    memset(s, 0, sizeof(*s));
    s->baud = 9600;
    s->datasize = 8;
    s->parity = 1;   // none
    s->stopsize = 1; // 1 stop bit
}

size_t rfc2217_greeting(rfc2217_t *s, uint8_t *out, size_t max) {
    rfc2217_io_t io = {.reply = out, .reply_max = max};
    put_cmd(&io, WILL, OPT_BINARY);
    put_cmd(&io, DO, OPT_BINARY);
    put_cmd(&io, WILL, OPT_SGA);
    put_cmd(&io, DO, OPT_SGA);
    put_cmd(&io, DO, OPT_COM_PORT);
    s->options_we |= bit(OPT_BINARY) | bit(OPT_SGA);
    s->options_they |= bit(OPT_BINARY) | bit(OPT_SGA);
    return io.reply_len;
}

// WILL/WONT/DO/DONT from the client. Answers only when our side's state changes, so the two ends
// can't bounce the same option back and forth.
static void on_verb(rfc2217_t *s, uint8_t verb, uint8_t opt, rfc2217_io_t *io) {
    if (opt == OPT_COM_PORT) {
        // Tracked apart (bit 44 doesn't fit): the client WILL, we DO. Always confirm a WILL.
        if (verb == WILL) put_cmd(io, DO, opt);
        else if (verb == DO) put_cmd(io, WONT, opt); // we're the server side, not a client
        return;
    }
    const uint32_t b = bit(opt);
    switch (verb) {
        case WILL: // they offer: accept what we know
            if (they_accept(opt)) {
                if (!(s->options_they & b)) put_cmd(io, DO, opt);
                s->options_they |= b;
            } else {
                put_cmd(io, DONT, opt);
            }
            break;
        case WONT:
            if (s->options_they & b) put_cmd(io, DONT, opt);
            s->options_they &= ~b;
            break;
        case DO: // they ask us to
            if (we_accept(opt)) {
                if (!(s->options_we & b)) put_cmd(io, WILL, opt);
                s->options_we |= b;
            } else {
                put_cmd(io, WONT, opt);
            }
            break;
        case DONT:
            if (s->options_we & b) put_cmd(io, WONT, opt);
            s->options_we &= ~b;
            break;
    }
}

static void on_subneg(rfc2217_t *s, rfc2217_io_t *io, uint8_t modem_state) {
    if (s->sb_overflow || s->sb_len < 2 || s->sb[0] != OPT_COM_PORT) return;
    const uint8_t cmd = s->sb[1];
    const uint8_t *arg = s->sb + 2;
    const size_t n = s->sb_len - 2;
    switch (cmd) {
        case CPO_SIGNATURE:
            // Empty: they ask for ours. Otherwise they tell theirs: nothing to answer.
            if (!n) put_cpo(io, cmd, (const uint8_t *)SIGNATURE, sizeof(SIGNATURE) - 1);
            break;
        case CPO_SET_BAUDRATE:
            if (n >= 4) {
                const uint32_t v = (uint32_t)arg[0] << 24 | (uint32_t)arg[1] << 16 | (uint32_t)arg[2] << 8 | arg[3];
                if (v) s->baud = v; // 0 only asks
                const uint8_t out[4] = {(uint8_t)(s->baud >> 24), (uint8_t)(s->baud >> 16), (uint8_t)(s->baud >> 8),
                                        (uint8_t)s->baud};
                put_cpo(io, cmd, out, 4);
            }
            break;
        case CPO_SET_DATASIZE:
        case CPO_SET_PARITY:
        case CPO_SET_STOPSIZE: {
            if (!n) break;
            uint8_t *field = cmd == CPO_SET_DATASIZE ? &s->datasize : cmd == CPO_SET_PARITY ? &s->parity : &s->stopsize;
            if (arg[0]) *field = arg[0]; // 0 only asks
            put_cpo(io, cmd, field, 1);
            break;
        }
        case CPO_NOTIFY_MODEMSTATE:
            put_cpo(io, cmd, &modem_state, 1);
            break;
        case CPO_NOTIFY_LINESTATE: {
            const uint8_t line_state = 0x60; // transmit holding and shift registers empty
            put_cpo(io, cmd, &line_state, 1);
            break;
        }
        case CPO_FLOWCONTROL_SUSPEND:
        case CPO_FLOWCONTROL_RESUME:
            break; // the client pausing our output: nothing to confirm
        case CPO_SET_CONTROL:
        case CPO_SET_LINESTATE_MASK:
        case CPO_SET_MODEMSTATE_MASK:
        case CPO_PURGE_DATA:
        default:
            if (n) put_cpo(io, cmd, arg, 1); // acknowledged as asked
            break;
    }
}

void rfc2217_input(rfc2217_t *s, const uint8_t *in, size_t len, rfc2217_io_t *io, uint8_t modem_state) {
    for (size_t i = 0; i < len; i++) {
        const uint8_t b = in[i];
        switch (s->st) {
            case ST_DATA:
                if (b == IAC) s->st = ST_IAC;
                else if (io->data_len < io->data_max) io->data[io->data_len++] = b;
                break;
            case ST_IAC:
                if (b == IAC) { // escaped 0xFF data byte
                    if (io->data_len < io->data_max) io->data[io->data_len++] = IAC;
                    s->st = ST_DATA;
                } else if (b >= WILL) { // WILL, WONT, DO, DONT
                    s->verb = b;
                    s->st = ST_VERB;
                } else if (b == SB) {
                    s->sb_len = 0;
                    s->sb_overflow = false;
                    s->st = ST_SB;
                } else {
                    s->st = ST_DATA; // NOP, GA, BRK...: nothing to do
                }
                break;
            case ST_VERB:
                on_verb(s, s->verb, b, io);
                s->st = ST_DATA;
                break;
            case ST_SB:
                if (b == IAC) s->st = ST_SB_IAC;
                else if (s->sb_len < RFC2217_SB_MAX) s->sb[s->sb_len++] = b;
                else s->sb_overflow = true;
                break;
            case ST_SB_IAC:
                if (b == IAC) { // escaped 0xFF inside a subnegotiation
                    if (s->sb_len < RFC2217_SB_MAX) s->sb[s->sb_len++] = IAC;
                    else s->sb_overflow = true;
                    s->st = ST_SB;
                } else if (b == SE) {
                    on_subneg(s, io, modem_state);
                    s->st = ST_DATA;
                } else {
                    s->st = ST_SB; // malformed: keep reading until IAC SE
                }
                break;
        }
    }
}

size_t rfc2217_modemstate(uint8_t modem_state, uint8_t *out, size_t max) {
    rfc2217_io_t io = {.reply = out, .reply_max = max};
    put_cpo(&io, CPO_NOTIFY_MODEMSTATE, &modem_state, 1);
    return io.reply_len;
}

size_t rfc2217_escape(const uint8_t *in, size_t len, uint8_t *out, size_t max, size_t *used) {
    size_t o = 0, i = 0;
    for (; i < len; i++) {
        const size_t need = in[i] == IAC ? 2 : 1;
        if (o + need > max) break;
        out[o++] = in[i];
        if (in[i] == IAC) out[o++] = IAC;
    }
    *used = i;
    return o;
}
