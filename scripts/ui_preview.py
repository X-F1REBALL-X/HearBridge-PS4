#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Desktop preview of the HearBridge PS4 web UI with mock status.
Usage: ui_preview.py [port] [fresh|scanning|idle|connecting|waiting|paging|nohs|scan_saved|-] [none|dirty|off|nogame|eqcustom]   then open http://127.0.0.1:<port>/
The second word picks the Equalizer / Game mode variant (default: a saved game is running and its sound is on).
"fresh" shows a first start: nothing saved, scan results waiting for a pick."""
import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

HTML = (Path(__file__).resolve().parent.parent / "src/web/index.html").read_bytes()
STATUS = {
    "version": "1.0.0", "ip": "192.0.2.10", "state": "streaming", "streaming": 1, "scanning": 0,
    "device": {"addr": "00:1A:7D:DA:71:13", "name": "Studio Headphones"},
    "codec": "SBC joint stereo", "rate": 48000, "bitpool": 45, "bitpool_max": 45,
    "buffer_ms": 78, "drops": 0, "pcm_rx": 5120, "pcm_idle_s": 0, "pcm_lost": 0, "tap_title": "CUSA55885", "resyncs": 0, "volume": 180, "hs_volume": {"avail": 1, "pct": 45}, "stream_s": 252, "error": "",
    "scan": [
        {"addr": "AC:80:0A:11:22:33", "name": "Coral Speaker", "kind": "speaker", "rank": 1, "rssi": -58},
        {"addr": "F4:7D:EF:44:55:66", "name": "Wireless Earbuds", "kind": "headset", "rank": 0, "rssi": -66},
        {"addr": "00:1B:66:77:88:99", "name": "JBL Flip 6", "kind": "speaker", "rank": 1, "rssi": -79},
    ],
    "saved": [
        {"addr": "00:1A:7D:DA:71:13", "name": "Studio Headphones", "kind": "headset", "last": 1},
        {"addr": "AC:80:0A:11:22:33", "name": "Coral Speaker", "kind": "speaker", "last": 0},
    ],
    "tap": 1, "skip": ["CUSA00011"],
    "log": ["Found adapter 0A12:0001", "Connecting to Studio Headphones (00:1A:7D:DA:71:13)",
            "Connected securely", "Streaming to Studio Headphones"],
}

if len(sys.argv) > 2 and sys.argv[2] == "fresh":
    STATUS.update({"state": "choose_device", "streaming": 0, "device": None, "codec": "",
                   "saved": [], "volume": 180, "stream_s": 0, "error": "",
                   "skip": [], "log": ["Found adapter 0A12:0001", "Scanning for devices",
                                       "Found 3 audio devices. Pick one on the HearBridge page."]})

if len(sys.argv) > 2 and sys.argv[2] == "scanning":
    STATUS.update({"state": "scanning", "streaming": 0, "device": None, "codec": "",
                   "scanning": 1, "scan_s": 6, "scan_phase": "inquiry",
                   "saved": [], "volume": 180, "stream_s": 0, "error": "", "skip": [],
                   "scan": [
                       {"addr": "00:1A:7D:DA:71:13", "name": "", "kind": "headset", "rank": 0, "rssi": -35},
                       {"addr": "14:EA:63:4F:76:EE", "name": "", "kind": "av", "rank": 2, "rssi": -72},
                       {"addr": "22:22:BE:55:48:DF", "name": "", "kind": "tv", "rank": 3, "rssi": -67},
                       {"addr": "22:22:3E:5B:AC:0C", "name": "", "kind": "tv", "rank": 3, "rssi": -68}],
                   "log": ["Found adapter 0A12:0001", "Scanning for headphones and speakers"]})

if len(sys.argv) > 2 and sys.argv[2] == "idle":
    STATUS.update({"state": "idle", "streaming": 0, "device": None, "codec": "", "stream_s": 0,
                   "error": "connect status 0x04 (page timeout (device not answering))",
                   "log": ["Disconnected. Pick a device or press Reconnect."], "scan": []})

if len(sys.argv) > 2 and sys.argv[2] == "connecting":
    STATUS.update({"state": "connecting", "streaming": 0, "codec": "", "stream_s": 0,
                   "log": ["Connecting to Studio Headphones (00:1A:7D:DA:71:13)"]})

if len(sys.argv) > 2 and sys.argv[2] == "waiting":
    STATUS.update({"state": "listening", "streaming": 0, "codec": "", "stream_s": 0,
                   "hs_volume": {"avail": 0, "pct": -1}, "error": "connect status 0x04 (page timeout (device not answering))",
                   "log": ["daemon: stream ended (reason 19): no paging, listening only until the headphones call or Connect / Try now"]})

if len(sys.argv) > 2 and sys.argv[2] == "scan_saved":
    STATUS.update({"state": "scanning", "streaming": 0, "scanning": 1, "scan_s": 5, "scan_phase": "inquiry",
                   "device": None, "codec": "", "stream_s": 0,
                   "log": ["Scanning for headphones and speakers"]})

if len(sys.argv) > 2 and sys.argv[2] == "paging":
    STATUS.update({"state": "reconnect_wait", "streaming": 0, "codec": "", "stream_s": 0,
                   "hs_volume": {"avail": 0, "pct": -1},
                   "log": ["daemon: page, 3 more after this one", "FAIL: connect status 0x04 (page timeout (device not answering))"]})

if len(sys.argv) > 2 and sys.argv[2] == "nohs":
    STATUS.update({"hs_volume": {"avail": 0, "pct": -1}})

SAVED_GAMES = [
    {"id": "CUSA55885", "name": "Demo Racing", "eq": {"on": 1, "db": [6, 3, 0, 0, 0]}, "boost": 250},
    {"id": "CUSA00007", "name": "Sample Fighter Collection", "eq": {"on": 1, "db": [-3, -1, 2, 5, 2]}, "boost": 200},
    {"id": "CUSA00341", "name": "", "eq": {"on": 0, "db": [0, 0, 0, 0, 0]}, "boost": 180},
]
STATUS["eq"] = {"on": 1, "db": [6, 3, 0, 0, 0]}
STATUS["volume"] = 250
STATUS["game"] = {"mode": 1, "run": "CUSA55885", "name": "Demo Racing", "has": 1, "active": 1, "dirty": 0, "saved": SAVED_GAMES}
variant = sys.argv[3] if len(sys.argv) > 3 else ""
if variant == "none":
    STATUS["eq"] = {"on": 0, "db": [0, 0, 0, 0, 0]}; STATUS["volume"] = 180
    STATUS["game"].update({"run": "CUSA00001", "name": "", "has": 0, "active": 0})
if variant == "dirty":
    STATUS["eq"] = {"on": 1, "db": [6, 3, 0, 2, 4]}; STATUS["volume"] = 300
    STATUS["game"].update({"dirty": 1})
if variant == "off":
    STATUS["eq"] = {"on": 0, "db": [0, 0, 0, 0, 0]}; STATUS["volume"] = 180
    STATUS["game"].update({"mode": 0, "active": 0})
if variant == "nogame":
    STATUS["eq"] = {"on": 1, "db": [-3, -1, 2, 5, 2]}; STATUS["volume"] = 200
    STATUS["game"].update({"run": "", "name": "", "has": 0, "active": 0})
if variant == "eqcustom":
    STATUS["eq"] = {"on": 1, "db": [4, -2, 1, 3, -5]}
    STATUS["game"].update({"run": "", "name": "", "has": 0, "active": 0})
if not STATUS.get("streaming"):
    STATUS["game"].update({"run": "", "name": "", "has": 0, "active": 0, "dirty": 0})
    if variant == "" or variant == "-":
        STATUS["eq"] = {"on": 0, "db": [0, 0, 0, 0, 0]}; STATUS["volume"] = 180

if not STATUS.get("streaming"):
    STATUS["hs_volume"] = {"avail": 0, "pct": -1}      # the daemon reports it only while streaming

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        self.do_GET()
    def do_GET(self):
        if self.path.startswith("/api/status"):
            body, t = json.dumps(STATUS).encode(), "application/json"
        elif self.path.startswith("/manifest.webmanifest"):
            import re
            src = (Path(__file__).resolve().parent.parent / "src/webui.c").read_text()
            m = re.search(r"k_manifest\[\] =\n((?:\".*\"\n?)+);", src)
            body, t = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))).encode().decode("unicode_escape").encode(), "application/manifest+json"
        elif self.path in ("/icon-192.png", "/icon-512.png", "/apple-touch-icon.png"):
            body, t = (Path(__file__).resolve().parent.parent / "src/web" / self.path[1:]).read_bytes(), "image/png"
        elif self.path.startswith("/api/cmd"):
            body, t = b'{"ok":true}', "application/json"
        else:
            body, t = HTML, "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", t)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

HTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 6767), H).serve_forever()
