// Reports the SVS's active input to a Cruller (RetroTINK 4K bridge).
//
// The RT4K cannot tell Cruller when the switch changes input, so the bridge
// does. Crullers announce themselves over mDNS as _rt4k._tcp (TXT: id, ver,
// api, name). The user picks one in the web UI; the bridge stores its id (not
// its address, which is looked up again by id) and sends it, over plain HTTP:
//
//   POST http://<host>:<port>/api/svs
//   {"id": "svs-bridge-...", "current_input": 3, "total_inputs": 8, "live": true,
//    "inputs": [{"kind": "scart", "name": "Super Nintendo / Super Famicom", "device": "snes"}, ...],
//    "output": {"kind": "component", "name": "RetroTINK 4K", "device": "rt4k"}}
//
// (from the layout: each input's module and the console on it, and the output
// to the RetroTINK 4K) on every input change, whenever the layout changes, as
// soon as it finds the Cruller
// (also when it announces itself again after a restart), and every 60 s in case
// a report was lost. Cruller answers {"ok": true, "changed": bool}, or 409 when it is
// already paired with another SVS Bridge.
//
// Requires NVS and mDNS (wifi_manager::start()) to be initialized.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>
#include "esp_err.h"

namespace cruller {

void start();

// A Cruller seen on the network
struct Found {
    std::string id;        // TXT id (its Pico id)
    std::string name;      // TXT name, empty while unnamed
    std::string version;   // TXT ver
    std::string instance;  // mDNS instance name, e.g. "Cruller Living"
    std::string host;      // e.g. "cruller-living" (without .local)
    std::string ip;        // IPv4, empty if the answer carried none
    uint16_t port;
    int64_t seen_ms;       // bridge uptime when last heard of
};

// The last report sent
struct Report {
    bool attempted;          // false until the first report
    bool ok;                 // Cruller accepted it
    int http_status;         // 0 if it could not be reached
    std::string error;       // why not, when !ok
    std::string paired_with; // on 409: the bridge id that Cruller is paired with
    std::string to_id;       // the Cruller it went to
    int input;               // the input reported
    int64_t at_ms;           // bridge uptime
    uint32_t count;          // reports accepted since the bridge started
};

struct State {
    std::string selected_id;   // the chosen Cruller, empty if none
    std::vector<Found> found;  // Crullers on the network, by name
    Report last;
};

State state();

// Browses the network now (blocks for about 2 s) and updates the list.
void scan();

// Chooses the Cruller to report to (by its id) and reports to it right away.
// An empty id stops reporting. Persists in NVS.
esp_err_t select(const std::string &id);

}  // namespace cruller
