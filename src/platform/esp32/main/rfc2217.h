// RFC 2217 server on TCP port 2217: the SVS's serial console for network clients, e.g. pyserial's
// "rfc2217://svs-bridge.local:2217", or a virtual COM port for the official SVS Management Utility.
// Up to 3 clients share it (a 4th replaces the oldest). It works like a serial cable: every client
// gets what the SVS says byte for byte (blank and repeated lines included, unlike the web UI's traffic
// log); what a client sends goes to the SVS as it comes (svs_usb::send_raw), unless the web UI is
// reading or writing its settings. A client raising DTR (opening a serial port) restarts the SVS, as
// the official utility expects, and gets its banner.
// Baud rate and the other line settings are acknowledged but change nothing (see rfc2217_proto.h).

#pragma once

#include <stddef.h>
#include <stdint.h>

// Call once the network stack is up.
void rfc2217_start(void);

// The connected clients' addresses, space-separated ("" when none), for /status.
void rfc2217_clients(char *out, size_t size);

// Connected clients, and the most allowed in *max.
int rfc2217_count(int *max);

// What each connected client has done, for the web UI (a snapshot; up to max, returns how many).
typedef struct {
    char ip[16];
    uint16_t port;
    uint32_t connected_s;   // since it connected
    uint32_t idle_s;        // since it last sent anything
    uint32_t rx, tx;        // bytes from / to it
    uint32_t commands;      // lines sent to the SVS
    uint32_t refused;       // lines not sent (SVS not connected, firmware update, ...)
} rfc2217_info_t;

int rfc2217_info(rfc2217_info_t *out, int max);
