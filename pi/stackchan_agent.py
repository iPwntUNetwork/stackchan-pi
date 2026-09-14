#!/usr/bin/env python3
"""
stackchan-agent — tiny REST companion for the Stack-chan CoreS3 firmware.

Runs on the Raspberry Pi Zero W 2 and gives the robot full local access to
itself: exec commands, status telemetry, camera snapshots, file listing.

Endpoints (all require header  X-Agent-Token: <STACKCHAN_TOKEN>):
  GET  /status            -> telemetry json
  POST /exec   {"cmd":..} -> run via /bin/bash, return {stdout, stderr, rc}
  GET  /camera           -> jpeg (if libcamera + camera present)
  GET  /files?path=..    -> directory listing json
  GET  /file?path=..     -> file contents (text)
  POST /say    {"text":..}-> speak via espeak-ng/pico2wave if installed
  POST /reboot           -> reboot the Pi
  GET  /health           -> {"ok":true}

Config via env:
  STACKCHAN_TOKEN   shared secret (default "stackchan" — change it!)
  STACKCHAN_PORT    listen port (default 8765)
  STACKCHAN_ALLOW_EXEC  set to "0" to disable /exec (default "1")

Stdlib only — no pip installs required. Install as a systemd service:
  sudo cp stackchan-agent.service /etc/systemd/system/
  sudo systemctl enable --now stackchan-agent
"""
import json
import os
import shutil
import subprocess
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOKEN = os.environ.get("STACKCHAN_TOKEN", "stackchan")
PORT = int(os.environ.get("STACKCHAN_PORT", "8765"))
ALLOW_EXEC = os.environ.get("STACKCHAN_ALLOW_EXEC", "1") == "1"
BOOT_TS = time.time()


def sh(cmd, timeout=20):
    try:
        p = subprocess.run(["/bin/bash", "-lc", cmd],
                           capture_output=True, text=True, timeout=timeout)
        return p.stdout, p.stderr, p.returncode
    except subprocess.TimeoutExpired:
        return "", "timeout", 124
    except Exception as e:  # noqa: BLE001
        return "", str(e), 125


def telemetry():
    out, _, _ = sh("cat /proc/uptime")
    uptime = float(out.split()[0]) if out.strip() else 0
    mem, _, _ = sh("free -m | awk '/Mem:/{print $3\"/\"$2\" MB\"}'")
    temp, _, _ = sh("vcgencmd measure_temp 2>/dev/null || cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null")
    disk, _, _ = sh("df -h / | awk 'NR==2{print $3\"/\"$2\" (\"$5\")\"}'")
    ssid, _, _ = sh("iwgetid -r 2>/dev/null")
    host, _, _ = sh("hostname")
    cam = shutil.which("libcamera-still") is not None or shutil.which("rpicam-still") is not None
    return {
        "hostname": host.strip(),
        "uptime_s": int(uptime),
        "mem": mem.strip(),
        "temp": temp.strip().replace("temp=", ""),
        "disk": disk.strip(),
        "wifi": ssid.strip(),
        "camera": cam,
        "ts": int(time.time()),
    }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # quiet
        pass

    def _auth(self):
        return self.headers.get("X-Agent-Token", "") == TOKEN

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            return self._json({"ok": True})
        if not self._auth():
            return self._json({"error": "unauthorized"}, 401)
        path, _, query = self.path.partition("?")
        if path == "/status":
            return self._json(telemetry())
        if path == "/camera":
            still = shutil.which("rpicam-still") or shutil.which("libcamera-still")
            if not still:
                return self._json({"error": "no camera tool"}, 404)
            tmp = "/tmp/stackchan_snap.jpg"
            _, err, rc = sh(f"{still} -o {tmp} -n -t 800 --width 1024 --height 768 2>/dev/null", timeout=10)
            if rc != 0 or not os.path.exists(tmp):
                return self._json({"error": err.strip()[:200] or "capture failed"}, 500)
            with open(tmp, "rb") as f:
                data = f.read()
            os.unlink(tmp)
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if path == "/files":
            q = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
            d = q.get("path", "/home")
            listing, err, rc = sh(f"ls -la --time-style=+%s {json.dumps(d)}")
            if rc != 0:
                return self._json({"error": err.strip()[:300]}, 400)
            return self._json({"path": d, "listing": listing})
        if path == "/file":
            q = dict(p.split("=", 1) for p in query.split("&") if "=" in p)
            fp = q.get("path", "")
            if not fp.startswith("/") or ".." in fp:
                return self._json({"error": "bad path"}, 400)
            try:
                with open(fp, "r", errors="replace") as f:
                    return self._json({"path": fp, "content": f.read(65536)})
            except OSError as e:
                return self._json({"error": str(e)}, 404)
        return self._json({"error": "not found"}, 404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        try:
            req = json.loads(self.rfile.read(n) or b"{}")
        except json.JSONDecodeError:
            return self._json({"error": "bad json"}, 400)
        if self.path == "/reboot":
            if not self._auth():
                return self._json({"error": "unauthorized"}, 401)
            self._json({"ok": True, "note": "rebooting"})
            sh("(sleep 1; sudo reboot) &", timeout=3)
            return
        if not self._auth():
            return self._json({"error": "unauthorized"}, 401)
        if self.path == "/exec":
            if not ALLOW_EXEC:
                return self._json({"error": "exec disabled"}, 403)
            cmd = str(req.get("cmd", ""))[:2000]
            if not cmd:
                return self._json({"error": "no cmd"}, 400)
            out, err, rc = sh(cmd, timeout=25)
            return self._json({"cmd": cmd, "stdout": out[:100000],
                               "stderr": err[:100000], "rc": rc})
        if self.path == "/say":
            text = str(req.get("text", ""))[:500]
            engine = shutil.which("pico2wave") and "pico" or (shutil.which("espeak-ng") and "espeak")
            if not engine:
                return self._json({"error": "no tts installed (apt install espeak-ng)"}, 404)
            if engine == "pico":
                sh(f'pico2wave -w /tmp/say.wav "{text}" && aplay -q /tmp/say.wav', timeout=20)
            else:
                sh(f'espeak-ng "{text}"', timeout=20)
            return self._json({"ok": True})
        return self._json({"error": "not found"}, 404)


if __name__ == "__main__":
    print(f"stackchan-agent listening on 0.0.0.0:{PORT} (exec={ALLOW_EXEC})")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
