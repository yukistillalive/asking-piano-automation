#!/usr/bin/env python3
"""
PianoAutomata bridge: browser display <-> ESP32 over USB serial.

- Serves index.html at http://localhost:8000
- Sends typed questions to the ESP32
- Follows the ESP32's serial output to know if it is thinking / playing / idle
- Keeps every question in questions.json (the page rotates through them)
- Survives unplugging/replugging the ESP32 and ESP32 restarts

Usage (macOS, keeps the laptop awake for the whole show):
    pip install pyserial
    caffeinate -dis python oracle.py                 # finds the ESP32 port itself
    caffeinate -dis python oracle.py /dev/cu.wchusbserial10

During the show you can:
- add words to blocklist.txt (one per line): matching questions disappear
  from the rotation, including ones already asked
- set "hidden": true on any entry in questions.json to hide it
Both files are re-read automatically; no restart needed.
"""
import json
import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial  # pip install pyserial
from serial.tools import list_ports

BAUD = 115200
HTTP_PORT = 8000
MAX_LEN = 140            # characters per question
BUSY_TIMEOUT_S = 150     # unlock the page if the ESP32 never reports back
ROTATION_LIMIT = 300     # newest N visible questions sent to the page

HERE = Path(__file__).resolve().parent
QUESTIONS_FILE = HERE / "data/questions.json"
BLOCKLIST_FILE = HERE / "blocklist.txt"
PORT_ARG = sys.argv[1] if len(sys.argv) > 1 else None
PORT_HINTS = ("wchusbserial", "usbserial", "SLAB_USBtoUART", "usbmodem",
              "ttyUSB", "ttyACM")


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


# ── questions.json + blocklist ─────────────────────────────
class Store:
    def __init__(self):
        self.lock = threading.Lock()
        self.items = []
        self.blocklist = []
        self.q_mtime = None
        self.b_mtime = None
        self.reload_if_changed()

    @staticmethod
    def _mtime(path):
        try:
            return path.stat().st_mtime
        except FileNotFoundError:
            return None

    def reload_if_changed(self):
        with self.lock:
            m = self._mtime(QUESTIONS_FILE)
            if m != self.q_mtime:
                self.q_mtime = m
                self.items = self._read_questions()
            m = self._mtime(BLOCKLIST_FILE)
            if m != self.b_mtime:
                self.b_mtime = m
                self.blocklist = self._read_blocklist()

    @staticmethod
    def _read_questions():
        if not QUESTIONS_FILE.exists():
            return []
        try:
            data = json.loads(QUESTIONS_FILE.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError) as e:
            log("questions.json unreadable, keeping it untouched:", e)
            return []
        items = []
        for q in data if isinstance(data, list) else []:
            if isinstance(q, str):
                items.append({"text": q})
            elif isinstance(q, dict) and q.get("text"):
                items.append(q)
        return items

    @staticmethod
    def _read_blocklist():
        if not BLOCKLIST_FILE.exists():
            return []
        words = []
        for line in BLOCKLIST_FILE.read_text(encoding="utf-8").splitlines():
            line = line.strip().lower()
            if line and not line.startswith("#"):
                words.append(line)
        return words

    def blocked(self, text):
        t = text.lower()
        return any(re.search(r"(?<!\w)" + re.escape(w) + r"(?!\w)", t)
                   for w in self.blocklist)

    def add(self, text):
        with self.lock:
            self.items.append({"text": text,
                               "time": time.strftime("%Y-%m-%d %H:%M:%S")})
            tmp = QUESTIONS_FILE.with_suffix(".json.tmp")
            tmp.write_text(json.dumps(self.items, ensure_ascii=False, indent=2),
                           encoding="utf-8")
            os.replace(tmp, QUESTIONS_FILE)   # atomic: never a half-written file
            self.q_mtime = self._mtime(QUESTIONS_FILE)

    def visible(self):
        with self.lock:
            texts = [q["text"] for q in self.items
                     if not q.get("hidden") and not self.blocked(q["text"])]
            return texts[-ROTATION_LIMIT:], len(self.items)


store = Store()

# ── serial link ────────────────────────────────────────────
state = {"status": "idle", "current": "", "busy_since": 0.0, "connected": False}
state_lock = threading.Lock()
ser = None
write_lock = threading.Lock()


def find_port():
    if PORT_ARG:
        return PORT_ARG
    for p in list_ports.comports():
        if any(h in p.device for h in PORT_HINTS):
            return p.device
        if os.name == "nt" and "USB" in (p.description or ""):
            return p.device
    return None


def connect():
    global ser
    warned = False
    while True:
        port = find_port()
        if port:
            try:
                ser = serial.Serial(port, BAUD, timeout=1)
                log("Serial connected:", port)
                with state_lock:
                    state["connected"] = True
                return
            except serial.SerialException as e:
                if not warned:
                    msg = str(e)
                    if "busy" in msg.lower():
                        msg += "  -> close the Arduino Serial Monitor / IDE"
                    log("Cannot open", port, "-", msg)
                    warned = True
        elif not warned:
            log("Waiting for the ESP32 to be plugged in...")
            warned = True
        time.sleep(2)


def read_serial():
    global ser
    while True:
        if ser is None:
            connect()
        try:
            raw = ser.readline()
        except (serial.SerialException, OSError) as e:
            log("Serial disconnected:", e)
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            with state_lock:
                state.update(connected=False, status="idle")
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        if not line:
            continue
        print("ESP32 |", line, flush=True)
        with state_lock:
            if line.startswith("Thinking"):
                if state["status"] == "idle":
                    state["busy_since"] = time.time()
                state["status"] = "thinking"
            elif line.startswith("Playing"):
                state["status"] = "playing"
            elif line.startswith("Waiting for input"):
                state["status"] = "idle"


def current_state():
    with state_lock:
        if (state["status"] != "idle"
                and time.time() - state["busy_since"] > BUSY_TIMEOUT_S):
            log("No reply from ESP32 for", BUSY_TIMEOUT_S, "s; unlocking the page")
            state["status"] = "idle"
        return dict(state)


def clean(text):
    text = re.sub(r"\s+", " ", text).strip()
    return text[:MAX_LEN]


# ── http ───────────────────────────────────────────────────
class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, ctype="text/plain; charset=utf-8"):
        data = body if isinstance(body, bytes) else body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self._send(200, (HERE / "index.html").read_bytes(),
                       "text/html; charset=utf-8")
        elif self.path == "/state":
            store.reload_if_changed()
            s = current_state()
            questions, total = store.visible()
            payload = {"status": s["status"], "current": s["current"],
                       "connected": s["connected"], "questions": questions,
                       "total": total}
            self._send(200, json.dumps(payload, ensure_ascii=False),
                       "application/json; charset=utf-8")
        else:
            self._send(404, "not found")

    def do_POST(self):
        if self.path != "/ask":
            return self._send(404, "not found")
        length = min(int(self.headers.get("Content-Length", 0)), 4096)
        text = clean(self.rfile.read(length).decode("utf-8", errors="replace"))
        if not text:
            return self._send(400, "empty question")
        with state_lock:
            if not state["connected"] or ser is None:
                return self._send(503, "oracle not connected")
            if state["status"] != "idle":
                return self._send(409, "busy")
            state.update(status="thinking", current=text, busy_since=time.time())
        try:
            with write_lock:
                ser.write((text + "\n").encode("utf-8"))
        except (serial.SerialException, OSError, AttributeError):
            with state_lock:
                state["status"] = "idle"
            return self._send(503, "oracle not connected")
        store.add(text)
        log("Question #%d: %s" % (len(store.items), text))
        self._send(200, "ok")

    def log_message(self, *args):
        pass  # keep the terminal for ESP32 output


if __name__ == "__main__":
    threading.Thread(target=read_serial, daemon=True).start()
    log(f"Open http://localhost:{HTTP_PORT} (double-click the page for fullscreen)")
    log(f"{len(store.items)} saved question(s) in {QUESTIONS_FILE.name}")
    try:
        ThreadingHTTPServer(("localhost", HTTP_PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        log("Stopped.")
