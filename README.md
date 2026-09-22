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
bridge only listens to the SVS, so the HD-15 can stay connected.

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

- Web UI → **Device** → **Factory reset**, or
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

In the web UI, **Bridge firmware update** → pick `build/svs_bridge.bin`. The
page shows its version, build date and SHA-256 before anything is sent; then
**Update bridge**. The image is checked again on the bridge (ESP-IDF app,
ESP32-S3, project `svs_bridge`, checksum) before it switches to it.

The new firmware is kept only once it connects to the WiFi network (5 minutes
by default, `menuconfig` → **SVS Bridge** → **Firmware updates**). If it
crashes or never connects, the bridge goes back to the previous firmware, so a
bad update cannot leave a remote bridge unreachable. After restarting, the web
UI tells whether the new version is running or was rolled back.

Bump `version.txt` for each release so the UI shows which one is running.

## Home Assistant

The bridge exposes a small read-only API for Home Assistant, so automations can
react to the active input changing (turn on a TV, load a RetroTINK profile, …).
No cloud, no MQTT — Home Assistant polls the bridge over the LAN.

- `GET /api/v1/info` — device identity (id, name, model, firmware) for setup.
- `GET /api/v1/state` — live SVS state (connected, current/total inputs,
  firmware) and bridge diagnostics (WiFi RSSI, uptime).

Both need an `Authorization: Bearer <token>` header. The token is shown in the
web UI under **Home Assistant** (copy or regenerate it there). The bridge also
advertises itself over mDNS (`_svsbridge._tcp`) so Home Assistant discovers it
automatically.

The matching custom integration (HACS) is in a separate repository:
[margaale/svs-bridge-hacs](https://github.com/margaale/svs-bridge-hacs).

## Build and flash

Requires ESP-IDF v6.x (developed with v6.1). From VS Code with the ESP-IDF
extension, or in a shell with ESP-IDF activated:

```
idf.py build
idf.py -p COMx flash monitor
```

Options live under `idf.py menuconfig` → **SVS Bridge** (serial settings,
hostname, setup network password).

## Flash layout

Two 6 MB OTA app slots plus a spare storage partition (see `partitions.csv`).
