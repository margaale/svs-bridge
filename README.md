# SVS Bridge

ESP32-S3 (N16R8) bridge to control an [SVS (Scalable Video Switch)](https://scalablevideoswitch.com/) remotely.

The ESP32-S3 acts as USB host on its native USB port and talks to the SVS's
CH340 at 9600 8N1 ([serial command reference](https://arthrimus.github.io/SVS_Firmware_Repository/serial.html)).

## Status

- [x] Stage 1: USB host, log everything the SVS sends, send commands from the console
- [x] Stage 2: WiFi captive portal, HTTPS web UI, OTA firmware upload
- [ ] Stage 3: SVS control from the web (live console) and an API with keys
- [ ] Upload a custom TLS certificate

## Hardware

| Board port | Use |
|---|---|
| `COM` (UART bridge) | Flashing and logs from the PC |
| `USB` (native, GPIO19/20) | USB host, connected to the SVS |

The SVS draws up to 1.5 A from its USB-C port. Power the board from a 5 V / 3 A
supply on the `5V` pin and bridge the `USB-OTG` solder jumper so the native USB
port outputs 5 V to the SVS. Do not bridge `IN-OUT`: its diode keeps the supply
from back-feeding the PC on the `COM` port.

## SVS serial link and the HD-15 to a RetroTINK

The SVS shares its single UART between the USB (CH340) and the HD-15 output it
uses to send serial commands to a RetroTINK. This has a hardware consequence,
confirmed by the SVS author:

- **Listening always works.** The SVS keeps transmitting its status (active
  input, firmware banner) to the bridge whether or not the HD-15 is connected,
  so reading the active input works during normal use.
- **Sending to the SVS needs the HD-15 disconnected.** With a RetroTINK
  connected over HD-15, commands to the SVS and firmware flashing do not work;
  the shared line corrupts what the SVS receives. Unplug the HD-15 to send
  commands or flash, then reconnect it.

For the intended use — watch the active input and act on the RetroTINK — the
bridge only needs to listen, so the HD-15 can stay connected. The bridge can
always send (the console, settings, firmware updates, RFC 2217 clients); what
it sends only gets through with the HD-15 unplugged. Restarting the SVS (a DTR
pulse, **SVS** → **Control** → **Restart SVS**) works either way.

## Your switch and the SVS's settings

The **SVS** tab shows the switch as its modules sit: the outputs, the control
module in the middle, the inputs. It replaces the official SVS Management
Utility for day-to-day settings:

- **What each module is.** The SVS only counts its inputs and recognises V3
  SCART/VGA modules and transcoders; pick the other modules (SCART, Component,
  VGA, S-Video/composite, D-Terminal) and add the outputs (SCART, Component,
  VGA, S-Video/composite, BNC, up to 6), and pick the console or device on
  each (it names the module): the list puts first the consoles that fit the module
  and warns when one does not send what the module takes. Drag a module to
  move it, inputs among inputs and outputs among outputs (Alt+←/→ with the
  keyboard). Transcoders move among the outputs too: one converts only the
  outputs past it, further from the control module. This is kept on the
  bridge; the active input's name and console also go to Home Assistant
  (`current_input_name`, `current_input_device`).
- **Settings stored in the SVS**, per input: RGB → YPbPr and YPbPr → RGB
  transcoding, sync on green and sync bypass (V3 modules), auto profile, and
  the IR codes the SVS sends the scaler (with the official utility's presets
  for the RetroTINK 4K, 5X and the OSSCs). **Read from the SVS**, change them,
  **Save to the SVS**. Moving an input takes its settings along, to be saved.
- **Input controls**, under the active input: Previous and Next, Seek (the
  previous or next input that has a signal), Go to an input, and Attract mode
  on or off (its state cannot be read). The same commands as the official
  utility's buttons; Previous, Next and Go need firmware 1.12, Seek and Attract
  mode 1.14. A light under each input shows the one on screen (green), and one
  under each transcoder (blue) that it is converting the input on screen, as
  last read from the SVS.

Reading and saving send commands, so the RetroTINK's HD-15 must be unplugged
(see above): the page asks first. It needs SVS firmware 1.20 or newer.

These use commands the SVS's serial documentation does not list, taken from
the official utility: `R<addr>` reads a byte of the control module's EEPROM,
`W<vvv><addr>` writes one, `Y<n>`/`G<n>` read the transcoders of input n. The
EEPROM map is in `src/core/svs_config.h`. The bridge writes only bytes it has
read and that change, reads each one back, and changes a transcoder by
switching to that input (as the utility does), then back to the one on screen.

## First setup

1. Power the bridge. With no WiFi configured it creates the open network
   `SVS-Bridge-XXXX`.
2. Join it from a phone or laptop; the setup page opens by itself (otherwise
   browse to `http://192.168.4.1`).
3. Pick your network and enter its password. Once the bridge connects, the
   setup network closes after a few seconds and the phone goes back to its
   usual network.
4. From your home network, open `https://svs-bridge.local`.

If the bridge cannot reach its network for 15 s (wrong password, router off,
moved house), the setup network comes back while it keeps retrying.

## Factory reset

Erases all settings (WiFi, TLS certificate) and reboots into setup mode; the
installed firmware stays. Either:

- Web UI → **Bridge** → **Factory reset**, or
- hold the **BOOT** button for 10 s while the bridge is running (the console
  announces it after 2 s; releasing earlier cancels).

Holding BOOT while powering up does not reset anything: that enters the
ESP32's download mode for flashing. The button GPIO and hold time are under
`menuconfig` → **SVS Bridge** → **Factory reset button**.

## HTTPS

On first boot the bridge generates its own ECDSA P-256 key and self-signed
certificate for `svs-bridge.local`, stored in NVS (it survives OTA updates; the
key never leaves the device). Browsers warn about it once; accept it. For
scripts, download it from the web UI (or `/device/cert`) and trust or pin it. Its
SHA-256 fingerprint is shown in the UI and printed on the console at boot.

Plain HTTP (port 80) only serves the setup portal while the setup network is
up, because phones detect captive portals over HTTP. Otherwise it redirects to
HTTPS.

## Firmware updates (OTA)

In the web UI, **Bridge** → **Bridge firmware**: pick a release and **Download and
install**, or **Install from a file…** with `build/esp32/svs_bridge.bin` (the page
shows its version, build date and SHA-256 before anything is sent). The image is checked again on the bridge (ESP-IDF app,
ESP32-S3, project `svs_bridge`, checksum) before it switches to it.

The new firmware is kept only once it connects to the WiFi network (5 minutes
by default, `menuconfig` → **SVS Bridge** → **Firmware updates**). If it
crashes or never connects, the bridge goes back to the previous firmware, so a
bad update cannot leave a remote bridge unreachable. After restarting, the web
UI tells whether the new version is running or was rolled back.


## Serial console over the network (RFC 2217)

The SVS's serial console is also on TCP port 2217 as an RFC 2217 server, for
tools such as pyserial (`rfc2217://svs-bridge.local:2217`). Up to 3 clients
share it like a serial cable: each gets what the SVS says byte for byte, and
what a client sends reaches the SVS as it comes, unless the web UI is reading
or writing its settings (they reach it only with the HD-15 unplugged). A client
that raises DTR, as opening a serial port does, restarts the SVS (video drops
for a moment) and gets the banner that follows.

This is meant to let the official SVS Management Utility configure the switch
over the network: create a virtual COM port that forwards to
`svs-bridge.local:2217` (for example com0com with com2tcp; it must pass DTR
on) and pick it in the utility. Its firmware update is not
supported through the bridge: use the web UI's.

**Bridge** → **Serial console clients** lists who is connected: address,
how long, when it last sent something, bytes both ways, and how many lines it
sent to the SVS and how many were not sent. Every connection, what it
negotiates and sends (in words: `SET-BAUDRATE 9600`, `text "R0"`) and why a
line was not passed on also go to the SVS tab's serial log.

## Home Assistant

The bridge exposes a small read-only API for Home Assistant, so automations can
react to the active input changing (turn on a TV, load a RetroTINK profile, …).
No cloud, no MQTT — Home Assistant polls the bridge over the LAN.

- `GET /api/v1/info` — device identity (id, name, model, firmware) for setup.
- `GET /api/v1/state` — live SVS state (connected, current/total inputs, the
  active input's name and console id, firmware) and bridge diagnostics (WiFi
  RSSI, uptime).

Both need an `Authorization: Bearer <token>` header. The token is shown in the
web UI's **Home Assistant** tab (copy or regenerate it there). The bridge also
advertises itself over mDNS (`_svsbridge._tcp`) so Home Assistant discovers it
automatically.

The matching custom integration (HACS) is in a separate repository:
[margaale/svs-bridge-hacs](https://github.com/margaale/svs-bridge-hacs).

## Cruller

[Cruller](https://github.com/margaale/Cruller) bridges a RetroTINK 4K to the
network, and its gameID applies the game profile of the console on screen. The
RT4K cannot tell it when the switch changes input, so the bridge does.

Crullers announce themselves over mDNS as `_rt4k._tcp` (TXT `id`, `ver`, `api`,
and `name` once named). The web UI's **Cruller** tab lists the ones on the
network; pick the one on the RetroTINK this switch feeds. The bridge stores its
`id` (not its address, which it looks up again) and sends it, over plain HTTP:

```
POST http://<host>:<port>/api/svs
Content-Type: application/json

{"id": "svs-bridge-aabbccddeeff", "current_input": 3, "total_inputs": 8, "live": true,
 "inputs": [{"kind": "scart", "name": "Super Nintendo / Super Famicom", "device": "snes"}, ...],
 "output": {"kind": "component", "name": "RetroTINK 4K", "device": "rt4k"}}
```

on every input change, whenever the layout changes, as soon as it finds the
Cruller (also when it announces itself again after a restart), and every 60 s in
case a report was lost (every 10 s while it cannot reach it). Nothing is sent
until the SVS has reported its input.

`inputs` is the SVS tab's layout: each input's module and the console or device
picked for it (`device` is its id in the web UI's list, `""` if none; an empty
list if no layout is saved). `output` is the output whose device is a RetroTINK
4K (`rt4k` or `rt4kce`), or `null` if none is. Cruller shows them in its own SVS
tab, the consoles as icons.

A Cruller keeps the first bridge that reports to it and answers others with
`409`; the tab then says it is paired with another bridge. Press **Unpair** in
that Cruller's own Cruller tab to move it to this one.

## Source layout

The same layout as [Cruller](https://github.com/margaale/Cruller):

- `src/core`: the pure logic, with no ESP-IDF — the AVR reset-vector patch
  (`svs_vectors.h`), the Intel HEX decoder (`svs_hex.h`), the SVS line parsing
  (`svs_protocol.h`) and the SVS's settings map (`svs_config.h`).
- `src/platform/esp32`: the ESP32-S3 target, an ESP-IDF project (`sdkconfig.defaults`,
  `partitions.csv`, `dependencies.lock`); its code in `main/`.
- `src/web`: the main page and the setup portal, and `embed.cmake`, which turns
  them into C arrays at build time.
- `src/version.cmake`: the version of local builds.
- `tests`: host unit tests of `src/core`. `scripts`: `build.sh` and the web UI's
  `dev_server.py`.

## Build and flash

Requires ESP-IDF v6.x (developed with v6.1). In a shell with ESP-IDF activated:

```
scripts/build.sh esp32
idf.py -C src/platform/esp32 -B build/esp32 -p COMx flash monitor
```

The build goes to `build/esp32`, with `svs_bridge-factory.bin` to flash a new
board at 0x0. Its `sdkconfig` is made from `sdkconfig.defaults` once and then
kept: after a change to the defaults, delete `build/esp32/sdkconfig` (the build
stops with that advice where it matters). Options live under
`idf.py -C src/platform/esp32 -B build/esp32 menuconfig` → **SVS Bridge** (serial
settings, hostname, setup network password).

To work on the web UI without a board, `python scripts/dev_server.py` serves the
pages with a mocked device API.

## Tests and CI

The pure logic that must never regress lives in dependency-free headers in
`src/core` and is unit-tested on the host, with no ESP-IDF or hardware:

```
tests/run.sh
```

## Releases

Work goes to `develop` (the default branch) through pull requests; `master` takes
what is released. Versions come from GitVersion (`GitVersion.yml`), and CI
(`.github/workflows/build.yml`) builds with them, the same scheme as
[Cruller](https://github.com/margaale/Cruller):

- **`master`:** every push is a release, tagged `vX.Y.Z`, with the images. The
  patch grows with each one; a line `+semver: minor` in a commit message bumps
  the minor.
- **`develop`:** every push is a pre-release, `vX.Y.Z-alpha.N`, to try on a
  board. N is CI's run number, which grows with every build on every branch: a
  newer build is always a newer version, and a release sorts after its
  pre-releases. The Bridge tab lists alphas but suggests them only to a bridge
  already running one.
- **Pull requests** (into `develop`): built as `X.Y.Z-pr.N`, not published; the
  images are on the run, one file each.

The images are named `svs-bridge-<version>-esp32s3_n16r8-<file>`:
`svs_bridge.bin` is the OTA image, `svs_bridge-factory.bin` flashes a new board
over USB at 0x0, and the bootloader, partition table and OTA data are there for
a flash in parts. Each release also carries a plain `svs_bridge.bin`, the name
bridges on 0.1.x look for when updating from GitHub.

CI passes the version to the build (`SVS_BRIDGE_VERSION`); a local build takes
it from `src/version.cmake`.

## Flash layout

Two 6 MB OTA app slots plus a spare storage partition (see `src/platform/esp32/partitions.csv`).
