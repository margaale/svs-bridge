"""Development server for the web UI, with a mocked device API.

Serves the pages in main/web as the bridge would, so the UI can be worked on
without hardware:

    http://localhost:8080/         index.html (the bridge serves it over HTTPS)
    http://localhost:8080/portal   portal.html (the setup portal, over HTTP)

The mock admin password is "password" (setting a new one in the setup flow
replaces it). A simulated SVS flash takes a few seconds.

Run with any Python 3:  python tools/dev_server.py [port]
"""

import hashlib
import json
import secrets
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

WEB = Path(__file__).resolve().parent.parent / "main" / "web"
# Served at /dev/svs_bridge.bin to try the bridge update form with a real image
BUILD_BIN = Path(__file__).resolve().parent.parent / "build" / "svs_bridge.bin"
PAGES = {"/": "index.html", "/portal": "portal.html"}

state = {
    "firmware": {"project": "svs_bridge", "version": "0.1.0", "built": "Sep 21 2026 12:00:00",
                 "idf": "v6.1", "partition": "ota_0"},
    "wifi": {"connected": True, "ssid": "Casa", "ip": "192.168.1.57", "rssi": -52,
             "hostname": "svs-bridge.local", "ap_active": False, "ap_ssid": "SVS-Bridge-3F2A"},
    "tls": {"source": "self-signed",
            "fingerprint": ":".join(f"{b:02X}" for b in bytes(range(0xA0, 0xC0)))},
    "svs": {"connected": True, "firmware": "SVS_FW_1.20", "current_input": 3, "total_inputs": 8,
            "live": False, "inputs_live": True, "send_enabled": False},
}

update = {
    "task": "idle", "phase": "", "progress": 0,
    "device": {"checked": False, "compatible": False, "vector": False, "vector_verified": False,
               "summary": "", "problem": "", "app_space": 0},
    "image": {"staged": False, "source": "", "size": 0, "sha256": ""},
    "can_flash": False, "blocker": "Check the SVS first",
    "result": "", "result_ok": False, "result_of": "idle",
}

# The real SVS is a vector bootloader (urboot v8.0); mock that. Set to "hw" to
# mock a plain hardware bootloader, or "bad" for an unsupported one.
MOCK_BOOTLOADER = "vector"

auth = {"password": "password", "sessions": set(),
        "api_token": "0123456789abcdef" * 4}  # 64 hex chars, like the firmware

START = time.monotonic()
svs_log = []  # {"seq", "t", "d", "s"}


def uptime_ms():
    return int((time.monotonic() - START) * 1000)


def log(d, s):
    svs_log.append({"seq": len(svs_log) + 1, "t": uptime_ms(), "d": d, "s": s})


def simulate_svs_traffic():
    """The SVS switching inputs on its own now and then."""
    log("*", "SVS connected (USB 1A86:7523, 9600 8N1)")
    for line in ("SVS_FW_1.20", "SVS CURRENT INPUT 3", "SVS TOTAL INPUTS 8"):
        log("<", line)
    n = 3
    while True:
        time.sleep(7)
        n = n % 8 + 1
        log("<", f"SVS NEW INPUT {n}")
        state["svs"]["current_input"] = n

NETWORKS = [
    {"ssid": "Casa", "rssi": -52, "secure": True},
    {"ssid": "Casa_5G", "rssi": -61, "secure": True},
    {"ssid": "Vecino", "rssi": -78, "secure": True},
    {"ssid": "Cafe libre", "rssi": -84, "secure": False},
]

RELEASES = [
    {"name": "SVS_FW_1.21.hex", "version": "1.21", "beta": False, "size": 84436},
    {"name": "SVS_FW_1.20.hex", "version": "1.20", "beta": False, "size": 84388},
    {"name": "SVS_FW_1.20_BETA.hex", "version": "1.20 BETA", "beta": True, "size": 83736},
    {"name": "SVS_FW_1.16.hex", "version": "1.16", "beta": False, "size": 78228},
]


def refresh_blocker():
    d, img = update["device"], update["image"]
    blocker = ""
    if update["task"] != "idle":
        blocker = "The SVS is busy"
    elif not d["checked"]:
        blocker = "Check the SVS first"
    elif not d["compatible"]:
        blocker = d["problem"]
    elif not img["staged"]:
        blocker = "Choose a firmware"
    elif img["size"] > d["app_space"]:
        blocker = f"The firmware ({img['size']} bytes) does not fit in the {d['app_space']} bytes below the bootloader"
    elif d["vector"] and not d["vector_verified"]:
        blocker = "Preview the vector patch first"
    update.update(blocker=blocker, can_flash=not blocker)


def finish(result, ok):
    update.update(result_of=update["task"], task="idle", phase="", result=result, result_ok=ok)
    refresh_blocker()


def stage(source, size):
    update["image"] = {"staged": True, "source": source, "size": size,
                       "sha256": hashlib.sha256(source.encode()).hexdigest()}
    if update["result_of"] == "flashing":
        update.update(result="", result_of="idle")
    refresh_blocker()


def clear():
    update["image"] = {"staged": False, "source": "", "size": 0, "sha256": ""}
    refresh_blocker()


def simulate_check():
    log("*", "Bootloader session started (restarting the SVS)")
    update.update(phase="Entering the bootloader")
    time.sleep(1)
    base = {"checked": True, "compatible": True, "vector": False, "vector_verified": False,
            "problem": "", "app_space": 32768 - 256}
    if MOCK_BOOTLOADER == "vector":
        update["device"] = {**base, "vector": True, "vector_num": 25,
                            "summary": "ATmega328P · urboot v8.0 · 256-byte bootloader · vector boot",
                            "problem": "This is a vector bootloader. Before flashing, run the preview."}
    elif MOCK_BOOTLOADER == "bad":
        update["device"] = {**base, "compatible": False, "app_space": 0,
                            "summary": "Classic STK500 bootloader (not urboot)",
                            "problem": "The SVS has a classic STK500 bootloader instead of urboot."}
    else:
        update["device"] = {**base,
                            "summary": "ATmega328P · urboot v7.7 · 384-byte bootloader · hardware boot"}
    log("*", "Bootloader session ended")
    update.update(phase="Waiting for the SVS to start")
    time.sleep(1.5)
    for line in (state["svs"]["firmware"], "SVS CURRENT INPUT 3", "SVS TOTAL INPUTS 8"):
        log("<", line)
    d = update["device"]
    finish("" if d["compatible"] else d["problem"], d["compatible"])


def simulate_preview():
    log("*", "Bootloader session started (restarting the SVS)")
    update.update(phase="Reading the reset vector")
    time.sleep(1.5)
    log("*", "Preview: chip reset 0xCF7F, bridge would write 0xCF7F (-> bootloader 0x7F00); "
             "current app vector 25 -> 0x0680")
    log("*", "Bootloader session ended")
    update["device"]["vector_verified"] = True
    finish("Preview OK: the bridge's reset vector matches what the official tool wrote. "
           "Flashing this SVS is safe.", True)


def simulate_flash():
    for phase, start, end in (("Entering the bootloader", 0, 0), ("Writing", 0, 50),
                              ("Verifying", 50, 100), ("Waiting for the SVS to start", 100, 100)):
        for p in range(start, end + 1, 5):
            update.update(phase=phase, progress=p)
            time.sleep(0.15)
    version = update["image"]["source"].split(" ")[0].removesuffix(".hex")
    state["svs"]["firmware"] = version if version.startswith("SVS_FW_") else "SVS_FW_1.21"
    finish(f"SVS firmware written and verified. The SVS now runs {state['svs']['firmware']}", True)


def start(task, fn):
    update.update(task=task, phase="Starting", progress=0, result="", result_of="idle")
    refresh_blocker()
    threading.Thread(target=fn, daemon=True).start()


class Handler(BaseHTTPRequestHandler):
    def send_body(self, code, body, ctype, cookie=None):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        if cookie is not None:
            self.send_header("Set-Cookie", f"svs_session={cookie}; Path=/; HttpOnly; SameSite=Strict")
        self.end_headers()
        self.wfile.write(data)

    def send_json(self, obj, code=200, cookie=None):
        self.send_body(code, json.dumps(obj), "application/json", cookie)

    def read_body(self):
        return self.rfile.read(int(self.headers.get("Content-Length", 0)))

    def logged_in(self):
        for part in self.headers.get("Cookie", "").split(";"):
            name, _, value = part.strip().partition("=")
            if name == "svs_session" and value in auth["sessions"]:
                return True
        return False

    def new_session(self):
        token = secrets.token_hex(32)
        auth["sessions"].add(token)
        return token

    def api_authorized(self):
        header = self.headers.get("Authorization", "")
        return header == "Bearer " + auth["api_token"]

    def svs_json(self):
        return {**state["svs"], "update": update}

    def do_GET(self):
        path = self.path.partition("?")[0]
        if path in PAGES:
            html = (WEB / PAGES[path]).read_text(encoding="utf-8")
            self.send_body(200, html, "text/html; charset=utf-8")
        elif path == "/api/status":
            state["firmware"].update(uptime_s=uptime_ms() // 1000, reset_reason="power-on")
            self.send_json({k: state[k] for k in ("firmware", "wifi", "tls", "svs")})
        elif path == "/dev/svs_bridge.bin" and BUILD_BIN.exists():
            self.send_body(200, BUILD_BIN.read_bytes(), "application/octet-stream")
        elif path == "/device/auth":
            self.send_json({"password_set": bool(auth["password"]), "authenticated": self.logged_in()})
        elif path in ("/api/v1/info", "/api/v1/state"):
            if not self.api_authorized():
                self.send_json({"error": "Invalid or missing API token"}, 401)
            elif path == "/api/v1/info":
                app = state["firmware"]
                self.send_json({"id": "svs-bridge-aabbccddeeff", "name": "SVS Bridge",
                                "model": "SVS Bridge (ESP32-S3)", "manufacturer": "SVS Bridge",
                                "sw_version": app["version"], "hostname": "svs-bridge.local",
                                "api_version": 1})
            else:
                self.send_json({"svs": state["svs"],
                                "bridge": {"rssi": state["wifi"]["rssi"],
                                           "uptime_s": uptime_ms() // 1000}})
        elif not self.logged_in():
            self.send_json({"error": "Login required"}, 401)
        elif path == "/device/scan":
            time.sleep(1.5)
            self.send_json(NETWORKS)
        elif path == "/device/api-token":
            self.send_json({"token": auth["api_token"]})
        elif path == "/device/releases":
            time.sleep(0.6)
            running = state["firmware"]["version"]
            size = BUILD_BIN.stat().st_size if BUILD_BIN.exists() else 1353024
            self.send_json({"running": running, "releases": [
                {"tag": "v0.3.0", "name": "v0.3.0", "notes_url": "https://github.com/margaale/svs-bridge/releases",
                 "prerelease": False, "size": size},
                {"tag": "v" + running, "name": "v" + running, "notes_url": "", "prerelease": False, "size": size},
                {"tag": "v0.4.0-alpha.2", "name": "v0.4.0-alpha.2", "notes_url": "", "prerelease": True, "size": size},
            ]})
        elif path == "/device/cert":
            self.send_body(200, "-----BEGIN CERTIFICATE-----\nMOCK\n-----END CERTIFICATE-----\n",
                           "application/x-pem-file")
        elif path == "/device/svs":
            self.send_json(self.svs_json())
        elif path == "/device/svs/log":
            query = self.path.partition("?")[2]
            after = int(dict(p.split("=", 1) for p in query.split("&") if "=" in p).get("after", 0))
            entries = [e for e in svs_log if e["seq"] > after][:100]
            self.send_json({"now": uptime_ms(), "entries": entries})
        elif path == "/device/svs/releases":
            time.sleep(1)
            self.send_json(RELEASES)
        else:
            self.send_body(404, "Not found", "text/plain")

    def do_POST(self):
        body = self.read_body()
        path = self.path
        if path == "/device/login":
            req = json.loads(body or b"{}")
            if req.get("password") == auth["password"]:
                self.send_json({"ok": True}, cookie=self.new_session())
            else:
                self.send_json({"error": "Wrong password"}, 401)
        elif path == "/device/setup-password":
            if auth["password"]:
                self.send_json({"error": "Password already set"}, 409)
            else:
                auth["password"] = json.loads(body or b"{}").get("password", "")
                self.send_json({"ok": True}, cookie=self.new_session())
        elif path == "/device/logout":
            auth["sessions"].clear()
            self.send_json({"ok": True}, cookie="")
        elif not self.logged_in():
            self.send_json({"error": "Login required"}, 401)
        elif path == "/device/wifi":
            req = json.loads(body or b"{}")
            state["wifi"].update(connected=True, ssid=req.get("ssid", ""), ip="192.168.1.57")
            self.send_json({"ok": True})
        elif path == "/device/ota":
            time.sleep(1)
            major, minor, patch = state["firmware"]["version"].split(".")
            state["firmware"]["version"] = f"{major}.{minor}.{int(patch) + 1}"
            part = state["firmware"]["partition"]
            state["firmware"]["partition"] = "ota_1" if part == "ota_0" else "ota_0"
            self.send_json({"ok": True})
        elif path == "/device/ota/github":
            time.sleep(2)
            tag = json.loads(body or b"{}").get("tag", "").lstrip("v")
            if tag:
                state["firmware"]["version"] = tag
            part = state["firmware"]["partition"]
            state["firmware"]["partition"] = "ota_1" if part == "ota_0" else "ota_0"
            self.send_json({"ok": True})
        elif path in ("/device/reboot", "/device/factory-reset"):
            self.send_json({"ok": True})
        elif path == "/device/api-token/regenerate":
            auth["api_token"] = secrets.token_hex(32)
            self.send_json({"token": auth["api_token"]})
        elif path == "/device/svs/mode":
            state["svs"]["send_enabled"] = bool(json.loads(body or b"{}").get("send"))
            self.send_json(self.svs_json())
        elif not state["svs"]["send_enabled"] and path in (
                "/device/svs/send", "/device/svs/restart", "/device/svs/check",
                "/device/svs/preview", "/device/svs/probe", "/device/svs/firmware/flash"):
            self.send_json({"error": "The bridge is in listen-only mode."}, 403)
        elif path == "/device/svs/restart":
            log("*", "SVS restarted")
            for line in (state["svs"]["firmware"], "SVS CURRENT INPUT 1", "SVS TOTAL INPUTS 8"):
                log("<", line)
            state["svs"].update(current_input=1, live=True)
            self.send_json({"ok": True})
        elif path == "/device/svs/check":
            start("checking", simulate_check)
            self.send_json(self.svs_json())
        elif path == "/device/svs/preview":
            start("previewing", simulate_preview)
            self.send_json(self.svs_json())
        elif path == "/device/svs/firmware":
            name = self.headers.get("X-File-Name", "uploaded file")
            if not body.lstrip().startswith(b":"):
                clear()
                self.send_body(400, "Line 1 is not a valid Intel HEX record", "text/plain")
                return
            stage(f"{name} (uploaded)", 30416)
            self.send_json(self.svs_json())
        elif path == "/device/svs/firmware/official":
            time.sleep(1.5)
            name = json.loads(body or b"{}").get("name", "")
            stage(f"{name} (official repository)", 30480)
            self.send_json(self.svs_json())
        elif path == "/device/svs/firmware/flash":
            if not update["can_flash"]:
                self.send_body(400, update["blocker"], "text/plain")
                return
            start("flashing", simulate_flash)
            self.send_json(self.svs_json())
        else:
            self.send_body(404, "Not found", "text/plain")

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.command, self.path))


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    threading.Thread(target=simulate_svs_traffic, daemon=True).start()
    print(f"Serving {WEB} on http://localhost:{port}/")
    ThreadingHTTPServer(("localhost", port), Handler).serve_forever()
