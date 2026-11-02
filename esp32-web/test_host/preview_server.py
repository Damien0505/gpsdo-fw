#!/usr/bin/env python3
"""Web interface preview without any hardware.

Browser <-> this server <-> the real ESP32 core logic (core_cli) <-> the GPSDO simulator.
The Arduino web layer is replaced by this file: the JSON comes from the code the ESP32 runs.

    python preview_server.py --cli ./core_cli.exe --port 8080

Log in with admin / admin. To misbehave: POST /__sim/loss?seconds=10, POST /__sim/encoder.
"""
import argparse
import base64
import hashlib
import json
import mimetypes
import os
import socket
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
from sim_gpsdo import SimGpsdo  # noqa: E402

WEB_ROOT = os.path.join(HERE, "..", "web")
PASSWORD = "admin"
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class Core:
    """core_cli, with whatever it transmits delivered to the simulator."""

    def __init__(self, path, sim):
        self.process = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.sim = sim
        self.lock = threading.RLock()
        self.silent_until = 0
        self.now = 1000

    def call(self, command):
        with self.lock:
            answer, sent = self._raw(command)
            for data in sent:
                if time.monotonic() < self.silent_until:
                    continue
                reply = self.sim.feed(data)
                if reply:
                    self.bytes(reply)
            return answer

    def _raw(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        sent = []
        while True:
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError("core_cli stopped")
            if line.startswith("tx "):
                sent.append(bytes.fromhex(line[3:].strip()))
            elif line.startswith("= "):
                return bytes.fromhex(line[2:].strip()).decode(), sent

    def bytes(self, data):
        self.call("b " + data.hex())

    def tick(self, now):
        self.now = now
        self.call("t %d" % now)


class Hub:
    def __init__(self):
        self.clients = []
        self.lock = threading.Lock()

    def add(self, connection):
        with self.lock:
            self.clients.append(connection)

    def send(self, text):
        frame = ws_frame(text)
        with self.lock:
            for client in list(self.clients):
                try:
                    client.sendall(frame)
                except OSError:
                    self.clients.remove(client)


def ws_frame(text):
    data = text.encode()
    if len(data) < 126:
        return bytes([0x81, len(data)]) + data
    if len(data) < 65536:
        return bytes([0x81, 126]) + struct.pack(">H", len(data)) + data
    return bytes([0x81, 127]) + struct.pack(">Q", len(data)) + data


def run_simulation(core, sim, hub, start):
    next_second = time.monotonic() + 1
    next_push = next_second
    while True:
        now = 1000 + start + int((time.monotonic() - run_simulation.t0) * 1000)
        core.tick(now)
        if time.monotonic() >= next_second:
            next_second += 1
            output = sim.second()
            if time.monotonic() >= core.silent_until:
                core.bytes(output)
        if time.monotonic() >= next_push:
            next_push += 1
            hub.send(core.call("status"))
            if core.call("changed") == "1":
                hub.send(core.call("config"))
        time.sleep(0.02)


def make_handler(core, sim, hub):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def respond(self, code, body, content_type="application/json", extra=()):
            data = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            for key, value in extra:
                self.send_header(key, value)
            self.end_headers()
            self.wfile.write(data)

        def authorized(self):
            expected = "Basic " + base64.b64encode(("admin:" + PASSWORD).encode()).decode()
            return self.headers.get("Authorization") == expected

        def do_GET(self):
            url = urlparse(self.path)
            path = url.path
            if path == "/ws":
                return self.websocket()
            if path == "/api/status":
                return self.respond(200, core.call("status"))
            if path == "/api/config":
                return self.respond(200, core.call("config"))
            if path == "/api/console":
                return self.respond(200, core.call("console"), "text/plain")
            if path == "/metrics":
                return self.respond(200, core.call("metrics"), "text/plain; version=0.0.4")
            if path == "/api/history":
                value = parse_qs(url.query).get("range", ["1h"])[0]
                if value not in ("1h", "24h"):
                    return self.respond(400, '{"ok":false,"error":"range is 1h or 24h"}')
                return self.respond(200, core.call("history " + value))
            if path in ("/setup", "/update"):
                return self.respond(200, "<p>Served by the ESP32 only: not part of this preview.</p>", "text/html")
            relative = "index.html" if path == "/" else path.lstrip("/")
            full = os.path.normpath(os.path.join(WEB_ROOT, relative))
            if not full.startswith(os.path.normpath(WEB_ROOT)) or not os.path.isfile(full):
                return self.respond(404, "Not found", "text/plain")
            with open(full, "rb") as source:
                content_type = mimetypes.guess_type(full)[0] or "application/octet-stream"
                return self.respond(200, source.read(), content_type)

        def do_POST(self):
            url = urlparse(self.path)
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length)
            if url.path == "/__sim/loss":
                core.silent_until = time.monotonic() + float(parse_qs(url.query).get("seconds", ["10"])[0])
                return self.respond(200, "{}")
            if url.path == "/__sim/encoder":
                core.bytes(sim.encoder_change())
                return self.respond(200, "{}")
            if url.path not in ("/api/config", "/api/action"):
                return self.respond(404, "Not found", "text/plain")
            if not self.authorized():
                return self.respond(401, "", "text/plain", [("WWW-Authenticate", 'Basic realm="GPSDO"')])
            if not (self.headers.get("Content-Type") or "").startswith("application/json"):
                return self.respond(415, '{"ok":false,"error":"Expected application/json"}')
            try:
                values = json.loads(body)
            except ValueError:
                return self.respond(400, '{"ok":false,"error":"Expected JSON"}')
            tickets = []
            if url.path == "/api/config":
                for key, value in values.items():
                    if type(value) is not int:
                        return self.respond(400, '{"ok":false,"error":"Values are integers"}')
                    tickets.append((key, core.call("set %s %d" % (key, value))))
            else:
                names = {"savepwm": "SAVEPWM", "forcesync": "FORCESYNC"}
                action = values.get("action")
                if action not in names:
                    return self.respond(400, '{"ok":false,"error":"Unknown action"}')
                tickets.append((action, core.call("cmd " + names[action])))
            results, all_ok = {}, True
            for key, ticket in tickets:
                while True:
                    answer = core.call("result " + ticket)
                    if answer != "pending":
                        break
                    time.sleep(0.05)
                if answer == "ok":
                    results[key] = {"ok": True}
                else:
                    results[key] = {"ok": False, "error": answer.split(" ", 1)[1]}
                    all_ok = False
            return self.respond(200, json.dumps({"ok": all_ok, "results": results}))

        def websocket(self):
            key = self.headers.get("Sec-WebSocket-Key", "")
            accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
            self.send_response(101)
            self.send_header("Upgrade", "websocket")
            self.send_header("Connection", "Upgrade")
            self.send_header("Sec-WebSocket-Accept", accept)
            self.end_headers()
            self.wfile.flush()
            self.connection.sendall(ws_frame(core.call("config")))
            hub.add(self.connection)
            try:
                while self.connection.recv(256):
                    pass
            except OSError:
                pass
            self.close_connection = True

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", required=True, help="path of the core_cli program")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--prefill", type=int, default=7200, help="seconds of history to fast-forward through")
    parser.add_argument("--warming", action="store_true", help="start at power-up instead of settled")
    args = parser.parse_args()

    sim = SimGpsdo()
    core = Core(args.cli, sim)
    hub = Hub()
    core.tick(1000)
    core.bytes(sim.boot())

    prefill = 0 if args.warming else args.prefill
    for second in range(prefill):
        core.tick(1000 + second * 1000)
        core.bytes(sim.second())
    # Answer boot time requests before going live
    core.tick(1000 + prefill * 1000)

    run_simulation.t0 = time.monotonic()
    threading.Thread(target=run_simulation, args=(core, sim, hub, prefill * 1000), daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(core, sim, hub))
    print("Preview at http://127.0.0.1:%d/ (admin / admin)" % args.port, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
