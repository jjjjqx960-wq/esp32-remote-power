#!/usr/bin/env python3
"""Serial bridge: relayd command queue -> ESP32 over USB serial.

ALTERNATIVE MODE: if your ESP32 is attached to a USB host (e.g. the PC it
controls, or a Raspberry Pi), this script long-polls relayd and writes each
command to the board's serial console — the firmware accepts PULSE / STATUS /
WOL <mac> on stdin, so the board does not even need Wi-Fi for commands.

Config via environment variables (no secrets in source):
  RELAY_BASE     e.g. https://relay.example.com/relay   (required)
  RELAY_TOKEN    shared token                            (required)
                 or RELAY_TOKEN_FILE pointing to a file containing it
  SERIAL_PORT    default COM3 (Windows) / /dev/ttyACM0 (Linux)
  BAUD           default 115200
"""
import json
import os
import ssl
import sys
import time
import urllib.request
import urllib.error

import serial

BASE = os.environ.get("RELAY_BASE", "").rstrip("/")
PORT = os.environ.get("SERIAL_PORT") or ("COM3" if os.name == "nt" else "/dev/ttyACM0")
BAUD = int(os.environ.get("BAUD", "115200"))
LOG = os.environ.get("BRIDGE_LOG", os.path.join(os.path.dirname(os.path.abspath(__file__)), "bridge.log"))


def load_token():
    tok = os.environ.get("RELAY_TOKEN")
    if tok:
        return tok.strip()
    tf = os.environ.get("RELAY_TOKEN_FILE")
    if tf:
        return open(tf).read().strip()
    sys.exit("set RELAY_TOKEN or RELAY_TOKEN_FILE")


ctx = ssl.create_default_context()


def log(msg):
    line = f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}"
    print(line, flush=True)
    try:
        with open(LOG, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def http(path, body=None, timeout=40):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout, context=ctx) as r:
        return json.loads(r.read())


def open_serial():
    while True:
        try:
            s = serial.Serial(PORT, BAUD, timeout=0.5)
            log(f"serial {PORT} open")
            return s
        except Exception as e:
            log(f"serial open fail: {e}; retry in 5s")
            time.sleep(5)


def main():
    if not BASE:
        sys.exit("set RELAY_BASE, e.g. https://relay.example.com/relay")
    token = load_token()
    ser = open_serial()
    while True:
        try:
            r = http(f"/poll?t={token}&wait=30")
        except Exception as e:
            log(f"poll error: {e}")
            time.sleep(3)
            continue
        cmd = r.get("cmd")
        if not cmd:
            continue
        log(f"cmd #{r.get('id')}: {cmd}")
        try:
            if not ser.is_open:
                ser = open_serial()
            ser.write((cmd + "\n").encode())
            ser.flush()
            log("serial write ok")
        except Exception as e:
            log(f"serial write fail: {e}")
            try:
                ser.close()
            except Exception:
                pass
            ser = open_serial()


if __name__ == "__main__":
    main()
