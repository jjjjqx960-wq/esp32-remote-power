#!/usr/bin/env python3
"""Durable, acknowledged ESP32 command relay (standard library only).

POST /cmd accepts the original {token, cmd} body, with optional target and ttl.
GET /poll?t=...&client=esp32&wait=25 leases a command for 60 seconds.
POST /ack?t=... accepts {id, client, attempt, ok, detail}; repeat ACKs are safe.
WOL/STATUS may be delivered at most three times. PULSE is never auto-replayed.
"succeeded" means the board acknowledged execution, not that a PC has booted.
"""

import argparse
import hmac
import json
import math
import os
from pathlib import Path
import re
import sqlite3
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


MAX_ID = 0xFFFFFFFF
ACTIVE = ("pending", "inflight")
REPLAYABLE = {"WOL", "STATUS"}


class RelayError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def normalize_command(value):
    if not isinstance(value, str) or len(value) > 128:
        raise RelayError(400, "bad cmd")
    parts = value.upper().split(maxsplit=1)
    if len(parts) == 1 and parts[0] in {"PULSE", "WOL", "STATUS"}:
        return parts[0]
    if len(parts) == 2 and parts[0] == "WOL":
        mac = re.sub(r"[:.\-\s]", "", parts[1])
        if re.fullmatch(r"[0-9A-F]{12}", mac):
            return "WOL " + ":".join(mac[i:i + 2] for i in range(0, 12, 2))
    raise RelayError(400, "bad cmd")


class CommandStore:
    """One process, synchronized threads; every transition commits before reply."""

    def __init__(self, path, *, capacity=16, lease=60, ttl=300,
                 max_attempts=3, clock=time.time, initial_id=None):
        if capacity < 1 or lease <= 0 or ttl <= 0 or max_attempts < 1:
            raise ValueError("capacity, lease, ttl and max_attempts must be positive")
        if initial_id is None:
            # Fresh installs avoid collisions with the old relay's small volatile IDs.
            initial_id = int(clock())
        if not 1 <= initial_id <= MAX_ID:
            raise ValueError("initial_id must fit uint32 and be positive")
        self.clock, self.capacity, self.lease = clock, capacity, lease
        self.ttl, self.max_attempts = ttl, max_attempts
        self.condition = threading.Condition(threading.RLock())
        self.db = sqlite3.connect(str(path), check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA journal_mode=WAL")
        self.db.execute("PRAGMA synchronous=FULL")
        self.db.executescript("""
            CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS commands (
                id INTEGER PRIMARY KEY, cmd TEXT NOT NULL, target TEXT NOT NULL,
                state TEXT NOT NULL, created REAL NOT NULL, expires REAL NOT NULL,
                attempts INTEGER NOT NULL DEFAULT 0, lease_until REAL,
                delivered REAL, completed REAL, ok INTEGER, detail TEXT);
            CREATE TABLE IF NOT EXISTS acks (
                id INTEGER NOT NULL, attempt INTEGER NOT NULL, ok INTEGER NOT NULL,
                detail TEXT NOT NULL, received REAL NOT NULL,
                PRIMARY KEY(id, attempt));
            CREATE TABLE IF NOT EXISTS clients (
                client TEXT PRIMARY KEY, last_poll REAL, last_ack REAL);
            CREATE INDEX IF NOT EXISTS command_state ON commands(state, id);
        """)
        with self.db:
            self.db.execute("INSERT OR IGNORE INTO meta VALUES ('next_id', ?)", (initial_id,))

    def close(self):
        with self.condition:
            self.db.close()

    def _finish(self, command_id, state, detail, now, ok=None):
        self.db.execute(
            "UPDATE commands SET state=?, detail=?, completed=?, ok=?, lease_until=NULL WHERE id=?",
            (state, detail, now, ok, command_id))

    def _expire(self, now):
        rows = self.db.execute(
            "SELECT * FROM commands WHERE state IN ('pending','inflight')").fetchall()
        for row in rows:
            if row["expires"] <= now:
                uncertain = row["state"] == "inflight" and row["cmd"] == "PULSE"
                self._finish(row["id"], "unknown" if uncertain else "failed",
                             "command_expired", now)
            elif row["state"] == "inflight" and row["lease_until"] <= now:
                if row["cmd"].split()[0] not in REPLAYABLE:
                    self._finish(row["id"], "unknown", "ack_timeout_not_replayed", now)
                elif row["attempts"] >= self.max_attempts:
                    self._finish(row["id"], "failed", "ack_timeout", now)
                else:
                    self.db.execute(
                        "UPDATE commands SET state='pending', lease_until=NULL, detail='ack_timeout_retry' WHERE id=?",
                        (row["id"],))

    def enqueue(self, command, target="esp32", ttl=None):
        command = normalize_command(command)
        if target != "esp32":
            raise RelayError(400, "unsupported target")
        ttl = self.ttl if ttl is None else ttl
        if isinstance(ttl, bool) or not isinstance(ttl, (int, float)) or not math.isfinite(ttl) or not 1 <= ttl <= 86400:
            raise RelayError(400, "ttl must be 1..86400 seconds")
        with self.condition, self.db:
            now = self.clock()
            self._expire(now)
            count = self.db.execute(
                "SELECT COUNT(*) FROM commands WHERE state IN ('pending','inflight')").fetchone()[0]
            if count >= self.capacity:
                raise RelayError(503, "queue full")
            command_id = self.db.execute("SELECT value FROM meta WHERE key='next_id'").fetchone()[0]
            if command_id > MAX_ID:
                raise RelayError(503, "command id space exhausted")
            self.db.execute("UPDATE meta SET value=? WHERE key='next_id'", (command_id + 1,))
            self.db.execute(
                "INSERT INTO commands(id,cmd,target,state,created,expires) VALUES (?,?,?,'pending',?,?)",
                (command_id, command, target, now, now + ttl))
            self.condition.notify_all()
            return {"ok": True, "id": command_id, "target": target, "state": "pending", "expires": now + ttl}

    def poll(self, client, wait=0):
        if client != "esp32":
            raise RelayError(409, "client=esp32 required; legacy bridge polling is disabled")
        if isinstance(wait, bool) or not isinstance(wait, (int, float)) or not math.isfinite(wait) or wait < 0:
            raise RelayError(400, "invalid wait")
        deadline = time.monotonic() + min(wait, 55)
        with self.condition:
            with self.db:
                self.db.execute(
                    "INSERT INTO clients(client,last_poll) VALUES (?,?) ON CONFLICT(client) DO UPDATE SET last_poll=excluded.last_poll",
                    (client, self.clock()))
            while True:
                with self.db:
                    now = self.clock()
                    self._expire(now)
                    inflight = self.db.execute(
                        "SELECT id FROM commands WHERE target=? AND state='inflight' LIMIT 1", (client,)).fetchone()
                    row = None if inflight else self.db.execute(
                        "SELECT * FROM commands WHERE target=? AND state='pending' ORDER BY id LIMIT 1", (client,)).fetchone()
                    if row:
                        attempt = row["attempts"] + 1
                        self.db.execute(
                            "UPDATE commands SET state='inflight', attempts=?, lease_until=?, delivered=? WHERE id=?",
                            (attempt, now + self.lease, now, row["id"]))
                        return {"ok": True, "cmd": row["cmd"], "id": row["id"],
                                "attempt": attempt, "lease_seconds": self.lease}
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return {"ok": True, "cmd": None, "id": 0}
                # Recheck leases/expiry even if no new enqueue or ACK wakes us.
                self.condition.wait(min(remaining, 1.0))

    def acknowledge(self, command_id, client, ok, attempt=None, detail=""):
        if client != "esp32":
            raise RelayError(409, "client=esp32 required")
        if type(command_id) is not int or not 1 <= command_id <= MAX_ID or type(ok) is not bool:
            raise RelayError(400, "id must be uint32 and ok must be boolean")
        if not isinstance(detail, str) or len(detail) > 240:
            raise RelayError(400, "detail must be a string of at most 240 characters")
        with self.condition, self.db:
            now = self.clock()
            self._expire(now)
            row = self.db.execute("SELECT * FROM commands WHERE id=?", (command_id,)).fetchone()
            if row is None:
                raise RelayError(404, "unknown command id")
            if not row["attempts"]:
                raise RelayError(409, "command has not been delivered")
            if attempt is None and row["attempts"] == 1:
                attempt = 1
            if type(attempt) is not int or not 1 <= attempt <= row["attempts"]:
                raise RelayError(400, "valid attempt required (echo poll response)")
            previous = self.db.execute(
                "SELECT ok FROM acks WHERE id=? AND attempt=?", (command_id, attempt)).fetchone()
            if previous:
                if previous["ok"] != ok:
                    raise RelayError(409, "conflicting acknowledgement")
                return {"ok": True, "id": command_id, "state": row["state"], "duplicate": True}
            self.db.execute("INSERT INTO acks VALUES (?,?,?,?,?)", (command_id, attempt, ok, detail, now))
            self.db.execute(
                "INSERT INTO clients(client,last_ack) VALUES (?,?) ON CONFLICT(client) DO UPDATE SET last_ack=excluded.last_ack",
                (client, now))
            stale = attempt < row["attempts"]
            if row["state"] != "succeeded":
                if ok:
                    # A delayed success still proves board execution; cancel retries.
                    self._finish(command_id, "succeeded", detail or "executed", now, True)
                elif not stale:
                    retry = (row["cmd"].split()[0] in REPLAYABLE and
                             row["attempts"] < self.max_attempts and now < row["expires"])
                    if retry:
                        self.db.execute(
                            "UPDATE commands SET state='pending', detail=?, ok=0, lease_until=NULL, completed=NULL WHERE id=?",
                            (detail or "execution_failed_retry", command_id))
                    else:
                        self._finish(command_id, "failed", detail or "execution_failed", now, False)
            state = self.db.execute("SELECT state FROM commands WHERE id=?", (command_id,)).fetchone()[0]
            self.condition.notify_all()
            return {"ok": True, "id": command_id, "state": state, "duplicate": False, "stale": stale}

    def status(self):
        with self.condition, self.db:
            now = self.clock()
            self._expire(now)
            active = [dict(row) for row in self.db.execute(
                "SELECT * FROM commands WHERE state IN ('pending','inflight') ORDER BY id")]
            results = [dict(row) for row in self.db.execute(
                "SELECT * FROM commands WHERE completed IS NOT NULL ORDER BY completed DESC,id DESC LIMIT 32")]
            last_ack = self.db.execute("SELECT * FROM acks ORDER BY received DESC,id DESC LIMIT 1").fetchone()
            last_delivery = self.db.execute(
                "SELECT id,cmd,attempts,delivered FROM commands WHERE delivered IS NOT NULL ORDER BY delivered DESC,id DESC LIMIT 1").fetchone()
            clients = {}
            for row in self.db.execute("SELECT * FROM clients"):
                age = None if row["last_poll"] is None else max(0, now - row["last_poll"])
                clients[row["client"]] = {"last_poll": row["last_poll"], "last_ack": row["last_ack"],
                                          "age": age, "online": age is not None and age <= 90}
            last = ({**results[0], "ts": results[0]["completed"]} if results else {"id": 0, "ts": 0})
            return {"ok": True, "pending": sum(row["state"] == "pending" for row in active),
                    "inflight": sum(row["state"] == "inflight" for row in active),
                    "last": last, "last_ack": dict(last_ack) if last_ack else None,
                    "last_delivery": dict(last_delivery) if last_delivery else None,
                    "commands": active, "results": results, "clients": clients,
                    "execution_note": "succeeded confirms board execution only; PC power state is not measured"}


PAGE = """<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>ESP32 Relay</title><button id=b>Wake PC</button><pre id=s>idle</pre>
<script>
const t=new URLSearchParams(location.search).get('t');
document.getElementById('b').onclick=async()=>{
 const s=document.getElementById('s');s.textContent='sending...';
 try{const r=await fetch('cmd',{method:'POST',headers:{'Content-Type':'application/json'},
 body:JSON.stringify({token:t,cmd:'WOL'})});const j=await r.json();
 s.textContent=j.ok?('queued #'+j.id+' (execution pending)'):('error: '+j.error);}
 catch(e){s.textContent='network error';}
};
</script>"""


class RelayServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, store, token):
        if not token:
            raise ValueError("empty relay token")
        self.store, self.token = store, token
        super().__init__(address, RelayHandler)

    def handle_error(self, request, client_address):
        if not isinstance(sys.exc_info()[1], (BrokenPipeError, ConnectionResetError, ConnectionAbortedError)):
            super().handle_error(request, client_address)


class RelayHandler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(65)

    def log_message(self, *_args):
        pass  # Request URLs contain credentials. Never log them.

    def _send(self, value, status=200, content_type="application/json"):
        data = value.encode() if isinstance(value, str) else json.dumps(value, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _request(self, post=False):
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)
        body = {}
        if post:
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if not 1 <= size <= 4096:
                    raise ValueError()
                body = json.loads(self.rfile.read(size))
                if not isinstance(body, dict):
                    raise ValueError()
            except (ValueError, UnicodeError):
                raise RelayError(400, "invalid JSON body") from None
        token = (query.get("t") or [body.get("token")])[0]
        if not isinstance(token, str) or not hmac.compare_digest(token.encode(), self.server.token.encode()):
            raise RelayError(403, "forbidden")
        return parsed.path, query, body

    def do_GET(self):
        try:
            path, query, _ = self._request()
            if path == "/":
                return self._send(PAGE, content_type="text/html; charset=utf-8")
            if path == "/status":
                return self._send(self.server.store.status())
            if path == "/poll":
                try:
                    wait = float((query.get("wait") or ["20"])[0])
                except ValueError:
                    raise RelayError(400, "invalid wait") from None
                return self._send(self.server.store.poll((query.get("client") or [None])[0], wait))
            raise RelayError(404, "not found")
        except RelayError as error:
            self._send({"ok": False, "error": str(error)}, error.status)

    def do_POST(self):
        try:
            path, _, body = self._request(post=True)
            if path == "/cmd":
                return self._send(self.server.store.enqueue(body.get("cmd"), body.get("target", "esp32"), body.get("ttl")))
            if path == "/ack":
                return self._send(self.server.store.acknowledge(
                    body.get("id"), body.get("client"), body.get("ok"), body.get("attempt"), body.get("detail", "")))
            raise RelayError(404, "not found")
        except RelayError as error:
            self._send({"ok": False, "error": str(error)}, error.status)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=59777)
    parser.add_argument("--token-file", type=Path, default=Path("/etc/relayd/token"))
    parser.add_argument("--state-file", type=Path, default=Path("/opt/relayd/state.sqlite3"))
    parser.add_argument("--capacity", type=int, default=16)
    parser.add_argument("--lease", type=int, default=60)
    parser.add_argument("--ttl", type=int, default=300)
    parser.add_argument("--max-attempts", type=int, default=3)
    args = parser.parse_args(argv)
    token = args.token_file.read_text(encoding="utf-8").strip()
    args.state_file.parent.mkdir(parents=True, exist_ok=True)
    if os.name == "posix":
        os.umask(0o077)
    store = CommandStore(args.state_file, capacity=args.capacity, lease=args.lease,
                         ttl=args.ttl, max_attempts=args.max_attempts)
    server = RelayServer((args.host, args.port), store, token)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        store.close()


if __name__ == "__main__":
    main()
