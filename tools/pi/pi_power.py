#!/usr/bin/env python3
"""pi_power -- let the robot's app shut down or restart this Pi cleanly.

Pulling the power on a running Pi can corrupt its SD card. With this service
running, the app's Settings -> Raspberry Pi -> "Shut down Pi" button asks the
robot (POST /v1/pi/power), and the robot asks this service:

    POST http://<this Pi>:8091/shutdown   header X-MPX-Token: <token>
    POST http://<this Pi>:8091/reboot     header X-MPX-Token: <token>
    GET  http://<this Pi>:8091/ping       (no token; says the service is up)

It answers 200 first and then runs `systemctl poweroff` (or reboot) a second
later, so the reply gets back to the robot before the network goes down.

Why the network and not the J-PI UART: the ROS bridge and RL policies
(mp3link) read that port, and a second reader would steal their bytes. The
robot already knows this Pi's IP from pi_announce / the bridge (HOST_INFO).

Settings come from the environment (/etc/default/pi-power):
    PI_POWER_PORT   default 8091, must match the firmware's APP_PI_POWER_PORT
    PI_POWER_TOKEN  default mpx-pi-power, must match APP_PI_POWER_TOKEN
Runs as root (it powers the machine off).
"""
import hmac
import json
import os
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(os.environ.get("PI_POWER_PORT", "8091"))
TOKEN = os.environ.get("PI_POWER_TOKEN", "mpx-pi-power")
DELAY_S = 1.0

COMMANDS = {
    "/shutdown": ["systemctl", "poweroff"],
    "/reboot": ["systemctl", "reboot"],
}


def run_later(cmd):
    def go():
        print("pi_power: running %s" % " ".join(cmd), flush=True)
        subprocess.run(["sync"], check=False)
        subprocess.run(cmd, check=False)
    t = threading.Timer(DELAY_S, go)
    t.daemon = False
    t.start()


class Handler(BaseHTTPRequestHandler):
    server_version = "pi_power/1"

    def _reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/ping":
            self._reply(200, {"ok": True})
        else:
            self._reply(404, {"ok": False, "error": "not_found"})

    def do_POST(self):
        # Drain any body so the client isn't left hanging.
        n = int(self.headers.get("Content-Length") or 0)
        if n:
            self.rfile.read(min(n, 4096))
        cmd = COMMANDS.get(self.path)
        if cmd is None:
            self._reply(404, {"ok": False, "error": "not_found"})
            return
        got = self.headers.get("X-MPX-Token", "")
        if not hmac.compare_digest(got.encode(), TOKEN.encode()):
            print("pi_power: refused %s from %s (bad token)"
                  % (self.path, self.client_address[0]), flush=True)
            self._reply(403, {"ok": False, "error": "bad_token"})
            return
        print("pi_power: %s requested by %s"
              % (self.path[1:], self.client_address[0]), flush=True)
        self._reply(200, {"ok": True, "action": self.path[1:]})
        run_later(cmd)

    def log_message(self, fmt, *args):
        pass                                            # quiet: we log above


def main():
    if TOKEN == "mpx-pi-power":
        print("pi_power: using the default token; set PI_POWER_TOKEN in "
              "/etc/default/pi-power (and APP_PI_POWER_TOKEN in the firmware)",
              flush=True)
    srv = HTTPServer(("0.0.0.0", PORT), Handler)
    print("pi_power: listening on port %d" % PORT, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
