// RFC 2217 server on TCP port 2217: the RT4K's serial console for network clients, e.g.
// pyserial's "rfc2217://cruller.local:2217" (Home Assistant's hass-RT4K). Up to 3 clients share it
// (a 4th replaces the oldest). Text only, shared with the web terminal: every client sees all the
// RT4K says, and each client's lines go to the RT4K whole, between Cruller's own transfers.

#pragma once

#include <stdbool.h>
#include <stddef.h>

void rfc2217_start(void);

// The connected clients' addresses, space-separated ("" when none), for /status.
void rfc2217_clients(char *out, size_t size);

// Connected clients, and the most allowed in *max.
int rfc2217_count(int *max);
