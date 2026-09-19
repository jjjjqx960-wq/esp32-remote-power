# DEPLOYMENT.md — Step-by-step deployment guide (human & AI operators)

This document is written to be executed literally, top to bottom, by an AI agent
or a human. Every step has:

- **Run** — the exact commands to execute
- **Expect** — what success looks like (verify before continuing)
- **If it fails** — symptom → cause → fix

Prerequisites you must already have (if any is missing, stop and ask the operator):

1. A Linux server reachable over the public internet with a domain name pointing
   at it (e.g. `relay.example.com`). A plain IP works too if you accept
   self-signed TLS — see Step 2 notes.
2. An ESP32-C3 board (Super Mini recommended, ~¥15 / ~$2) + a **data-capable**
   USB cable (many cheap cables are power-only — this is the #1 flashing failure).
3. Root/sudo on the server. Local machine needs Python 3.9+ and ESP-IDF v5.x.

> **Convention**: `<PLACEHOLDER>` means "replace before running". Never commit
> real tokens/passwords — they live only in `/etc/relayd/token` and `sdkconfig`.

---

## Phase A — Server (relayd)

### A1. Install relayd

Fastest path — run the idempotent installer as root:

```bash
curl -fsSL https://raw.githubusercontent.com/jjjjqx960-wq/esp32-remote-power/main/deploy/server-setup.sh | sudo bash
```

Or from a clone: `sudo bash deploy/server-setup.sh`

**Expect**: prints `relayd is running on 127.0.0.1:59777` and a generated token
line. The script is safe to re-run.

Manual equivalent (what the script does):

```bash
sudo install -d -m 700 /etc/relayd /opt/relayd
sudo cp server/relayd.py /opt/relayd/
openssl rand -hex 20 | sudo tee /etc/relayd/token
sudo chmod 600 /etc/relayd/token
sudo cp server/relayd.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now relayd
```

**Expect**: `systemctl is-active relayd` → `active`.

**If it fails**:

| Symptom | Fix |
|---|---|
| `Failed to start` / port in use | `ss -tlnp \| grep 59777` — change `--port` in the unit or kill the squatter |
| token file missing | re-run the `openssl rand` line |

### A2. Verify relayd locally

```bash
TOKEN=$(sudo cat /etc/relayd/token)
curl -s "http://127.0.0.1:59777/status?t=$TOKEN"
```

**Expect**: JSON containing `"ok":true`.

### A3. HTTPS front (pick ONE)

The ESP32 uses the ESP-IDF certificate bundle, so use a real CA cert.

**Option A3a — Caddy (simplest, auto-TLS; recommended for AI operators):**

```bash
# Debian/Ubuntu: apt install caddy  (or: snap / docker)
sudo tee /etc/caddy/Caddyfile <<'EOF'
relay.example.com {
    reverse_proxy /relay/* 127.0.0.1:59777
}
EOF
sudo systemctl reload caddy
```

**Option A3b — nginx + certbot:**

```bash
sudo certbot --nginx -d relay.example.com
# then merge server/nginx-relay.conf.example into your site config
sudo nginx -t && sudo systemctl reload nginx
```

**Expect**: `curl -s "https://relay.example.com/relay/status?t=$TOKEN"` → `"ok":true`.

**If it fails**: DNS not propagated (`dig +short relay.example.com`), firewall
blocking 443 (`ufw allow 443`), or certbot rate limit (wait 1h).

> **No domain?** You can use a self-signed cert, but then you must either append
> your CA to the IDF cert bundle or accept that `crt_bundle_attach` fails.
> Strongly prefer getting a free domain (DuckDNS etc.) — it's less work.

---

## Phase B — Firmware

### B1. Install ESP-IDF v5.x (skip if installed)

Linux/macOS:

```bash
git clone -b v5.4.2 --recursive https://github.com/espressif/esp-idf ~/esp-idf
cd ~/esp-idf && ./install.sh esp32c3 && . ./export.sh
```

Windows (PowerShell): install "ESP-IDF Tools" from espressif.com, or use the
ESP-IDF PowerShell shortcut which already has the environment loaded.

**Expect**: `idf.py --version` prints `ESP-IDF v5.x`.

### B2. Configure — fill `firmware/sdkconfig.defaults`

Edit these 6 lines (or run `idf.py menuconfig` → "ESP32 Remote Power"):

| Key | Value |
|---|---|
| `CONFIG_RELAY_WIFI_SSID` | your **2.4 GHz** SSID (ESP32-C3 cannot see 5 GHz) |
| `CONFIG_RELAY_WIFI_PASSWORD` | Wi-Fi password |
| `CONFIG_RELAY_WIFI_COUNTRY` | `"CN"` / `"US"` / your country code |
| `CONFIG_RELAY_WIFI_TX_POWER` | keep `60` (15 dBm) — see warning below |
| `CONFIG_RELAY_BASE_URL` | `https://relay.example.com/relay` (no trailing slash) |
| `CONFIG_RELAY_TOKEN` | same string as `/etc/relayd/token` |
| `CONFIG_RELAY_WOL_DEFAULT_MAC` | your PC's NIC MAC, `aa:bb:cc:dd:ee:ff` |

> ⚠️ **Do not raise `CONFIG_RELAY_WIFI_TX_POWER`.** On Super Mini boards the
> IDF default (~20 dBm = `80`) is exactly why "Wi-Fi won't connect". `60` (15 dBm)
> is the verified fix; `40` (10 dBm) is the fallback. See
> `docs/WIFI-TX-POWER-FIX.md`.

### B3. Build & flash

```bash
cd firmware
idf.py set-target esp32c3
idf.py build
idf.py -p <PORT> flash monitor    # Windows: COMx · Linux: /dev/ttyACMx · macOS: /dev/cu.usbserial-* or /dev/cu.usbmodem*
```

**Expect** in monitor output:

```
Wi-Fi connected; IP=192.168.x.x GW=192.168.x.1
relay poll task ready -> https://relay.example.com/relay
```

**If it fails**:

| Symptom | Cause → Fix |
|---|---|
| `Could not open <PORT>` | cable is power-only → swap cable; or another monitor/bridge holds the port → kill it |
| `reason=2` loops (auth expire) | TX power still too high → set `CONFIG_RELAY_WIFI_TX_POWER=40` and reflash |
| `reason=15` / `201` | wrong password → fix SSID/password; `201` = SSID not found (maybe 5 GHz) |
| `relay poll fail: err=...` | HTTPS/token problem → verify A3 curl from server; check `CONFIG_RELAY_TOKEN` matches `/etc/relayd/token` |
| connects then immediately drops | country code mismatch → set `CONFIG_RELAY_WIFI_COUNTRY` to your real country |

### B4. Verify end-to-end

On the server (or any machine with the token):

```bash
TOKEN=$(sudo cat /etc/relayd/token)
curl -s -X POST https://relay.example.com/relay/cmd \
  -H 'Content-Type: application/json' \
  -d "{\"token\":\"$TOKEN\",\"cmd\":\"STATUS\"}"
```

**Expect**: `{"ok":true,"id":N}`. Within ~1 s the board serial log shows
`relay cmd id=N: STATUS` → `relay ack id=N ok=true`.

---

## Phase C — Client

### C1. Install esp32ctl (on the server or any Linux/macOS box)

```bash
sudo install -m 755 client/esp32ctl /usr/local/bin/esp32ctl
# if not on the relayd host, point it at the HTTPS endpoint instead:
export RELAYD_URL="https://relay.example.com/relay"
export RELAYD_TOKEN_FILE=/path/to/token   # copy of /etc/relayd/token
```

Usage:

```bash
esp32ctl wol                    # wake default MAC
esp32ctl wol 11:22:33:44:55:66
esp32ctl pulse                  # GPIO3 400ms press (needs wiring, Phase D)
```

Or just curl — the whole protocol is 4 endpoints (see README §Protocol).

### C2. Web button

`https://relay.example.com/relay/?t=<token>` → big red **Wake PC** button.
Bookmark it on your phone.

---

## Phase D — Optional: GPIO3 physical power button

Only needed when WoL is impossible (BIOS WoL off, PC on Wi-Fi, etc.).
WoL-only users skip this phase entirely — the board just needs USB power
inside your home.

Wiring (optocoupler/relay module recommended, ~¥3):

```
ESP32 GPIO3 ──> relay module IN ──> relay COM/NO ──> motherboard F_PANEL PWR_SW+ / PWR_SW-
ESP32 GND   ──> relay module GND
```

- `PULSE` closes GPIO3 for 400 ms = one short power-button press.
- Default logic is active-low (`RELAY_ACTIVE_LOW=1`). If your module triggers on
  HIGH, rebuild: `idf.py build -DRELAY_ACTIVE_LOW=0 && idf.py flash`.
- Also enable "power on after AC loss" in BIOS if you want full recovery after
  outages.

## Phase E — Enable WoL on the target PC (one-time)

1. BIOS/UEFI: enable *Wake-on-LAN* / *PME* / *Power On by PCIe*.
2. OS NIC driver: enable *Wake on Magic Packet* (Windows: Device Manager → NIC →
   Power Management + Advanced tab; Linux: `ethtool -s eth0 wol g`).
3. The ESP32 and the PC must be on the **same L2 broadcast domain** (same VLAN/
   subnet). WoL does not cross routers.

## Verification matrix

| Check | Command / action | Pass criteria |
|---|---|---|
| relayd alive | `curl .../status?t=TOKEN` | `"ok":true` |
| board online | same `/status` JSON | `clients.esp32.online == true` (last_poll ≤ 90 s) |
| command flow | `esp32ctl status` | board log shows cmd + ack |
| WoL | `esp32ctl wol` with PC off | PC powers on |
| Pulse | `esp32ctl pulse` with PC off | PC powers on |

## Troubleshooting quick index

| Where | Symptom | First thing to check |
|---|---|---|
| Board | `reason=2` auth loop | TX power → 60 or 40 (the Super Mini bug) |
| Board | can't see SSID | 2.4 GHz only; hidden SSID not supported by this config |
| Board | `poll fail` every cycle | `CONFIG_RELAY_BASE_URL`/`TOKEN`, HTTPS cert valid |
| Server | curl `/status` 403 | wrong token |
| Server | 502 via nginx | relayd down (`systemctl status relayd`) |
| PC | WoL no effect | same subnet? BIOS+driver WoL enabled? right MAC? |
