# esp32-remote-power

Remote power-on for your PC with a **~¥15 / ~$2 ESP32-C3 Super Mini** — Wake-on-LAN
magic packets plus an optional GPIO "physical power button" pulse, driven over the
internet through a tiny self-hosted command relay.

```
[you] --esp32ctl / web button--> [relayd on your VPS] <--HTTPS long-poll-- [ESP32-C3]
                                                                          |
                                            +-----------------------------+----------------+
                                            |                                              |
                                     WOL magic packet (UDP:9)                     GPIO3 pulse 400ms
                                            |                                              |
                                       PC NIC wakes it                     motherboard PWR_SW pins
```

## Features

- **WoL over Wi-Fi**: `WOL <mac>` sends the magic packet to both the subnet-directed
  broadcast and `255.255.255.255`, 3× each, with a configurable default target MAC.
- **GPIO3 power-button pulse**: `PULSE` drives GPIO3 for 400 ms — wire it to a
  relay/opto-coupler on the motherboard `PWR_SW` header for machines where WoL is
  unavailable (BIOS off, Wi-Fi-only NIC, etc.).
- **Durable command relay**: `server/relayd.py` is a stdlib-only Python daemon with
  SQLite persistence, command leases, ACK/retry (3 attempts) and per-client status —
  commands survive board reboots and server restarts.
- **Everything secret lives in config**: Wi-Fi credentials, relay URL, token and the
  default MAC are all `menuconfig`/`sdkconfig` options — nothing is hardcoded.
- **No cloud dependency**: one small VPS (or any HTTPS host) + nginx + this daemon.

## ⚠️ ESP32-C3 Super Mini won't connect to Wi-Fi? Lower the TX power.

This was the single hardest bug in this project, and it's the reason this section
exists: **many ESP32-C3 Super Mini boards cannot join a 2.4 GHz AP at the ESP-IDF
default TX power (~20 dBm).** The symptom is very specific:

- `scan()` sees the AP fine (RSSI is great — *downlink* works)
- connect fails ~4 s later with `reason: 2` (`AUTH_EXPIRE`) — the AP never answers
  the 802.11 auth frame, because your board's *uplink* is too hot for its
  marginal RF layout/antenna
- no WPA2/WPA3/PMF/auth-mode setting makes any difference (we tested the whole
  matrix on native IDF)

**Fix: drop max TX power to 15 dBm.** This project does it for you:

```c
esp_wifi_set_max_tx_power(CONFIG_RELAY_WIFI_TX_POWER);   // 60 = 60×0.25 = 15 dBm
```

`CONFIG_RELAY_WIFI_TX_POWER` defaults to `60` (15 dBm) in `sdkconfig.defaults` —
that's the value that took our Super Mini from *never connects* to *connects in
seconds*. If 15 dBm is still not enough on your board, try `40` (10 dBm).

Full write-up with the diagnostic method: [docs/WIFI-TX-POWER-FIX.md](docs/WIFI-TX-POWER-FIX.md)

## Repository layout

```
firmware/            ESP-IDF project (ESP32-C3): Wi-Fi STA + relay poller + WoL + GPIO3 pulse
  main/main.c        all firmware logic (~600 lines, single file)
  main/Kconfig.projbuild   every option: SSID/password/country/TX power/relay URL/token/MAC
  sdkconfig.defaults       template — fill in your values before flashing
server/relayd.py     stdlib-only command queue daemon (SQLite, lease+ACK+retry)
server/test_relayd.py      unit tests for the store
server/relayd.service      systemd unit
server/nginx-relay.conf.example   HTTPS reverse-proxy snippet
client/esp32ctl    bash CLI: esp32ctl wol | pulse | status
bridge/relay_bridge.py     OPTIONAL serial-bridge mode (board on USB, no Wi-Fi needed)
docs/WIFI-TX-POWER-FIX.md  the Super Mini TX-power investigation in detail
```

## Quick start

### 1. Server (your VPS)

```bash
sudo install -d -m 700 /etc/relayd /opt/relayd
sudo cp server/relayd.py /opt/relayd/
openssl rand -hex 20 | sudo tee /etc/relayd/token && sudo chmod 600 /etc/relayd/token
sudo cp server/relayd.service /etc/systemd/system/
sudo systemctl enable --now relayd
```

Put nginx (or any TLS proxy) in front using `server/nginx-relay.conf.example`,
mapping `/relay/` → `http://127.0.0.1:59777/`. **Do not expose relayd over plain
HTTP** — the token is in the request.

### 2. Firmware

```bash
cd firmware
# edit sdkconfig.defaults: SSID, password, relay URL, token (same as server),
# default WoL MAC = your PC's MAC. Or: idf.py menuconfig -> "ESP32 Remote Power"
idf.py set-target esp32c3
idf.py build
idf.py -p COM3 flash monitor     # COM3 / /dev/ttyACM0 depending on OS
```

Boot log should show `Wi-Fi connected; IP=...` within seconds. If it loops with
`reason=2`, lower `CONFIG_RELAY_WIFI_TX_POWER` (see the warning above).

### 3. Push the button

```bash
esp32ctl wol             # magic packet to CONFIG_RELAY_WOL_DEFAULT_MAC
esp32ctl wol 11:22:33:44:55:66   # explicit target
esp32ctl pulse           # 400ms GPIO3 pulse (physical power button)
esp32ctl status          # queue/board status
```

Or open `https://your-server/relay/?t=<token>` — there's a big red web button.

### 4. Optional: GPIO3 → motherboard

`PULSE` toggles GPIO3 for 400 ms. Typical wiring: GPIO3 → optocoupler/relay →
the two `PWR_SW` pins on the motherboard front-panel header. The relay logic
level is selectable at build time:

```bash
idf.py build -DRELAY_ACTIVE_LOW=1   # default: active-low (L jumper style)
```

### 5. Optional: serial-bridge mode (no Wi-Fi on the board)

If the board is plugged into a USB host, run `bridge/relay_bridge.py` there —
it polls relayd and types commands into the board's serial console:

```bash
RELAY_BASE=https://relay.example.com/relay RELAY_TOKEN_FILE=/etc/relayd/token \
    python3 bridge/relay_bridge.py
```

## Protocol

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/cmd`   | POST `{token, cmd}` | enqueue `PULSE`/`STATUS`/`WOL [mac]` |
| `/poll?wait=N&client=esp32` | GET | board long-polls; leases a command |
| `/ack`   | POST `{id, client, attempt, ok, detail}` | board reports execution result |
| `/status`| GET | queue + per-client online status |
| `/`      | GET | web button page |

Commands are deduplicated, leased for 60 s, retried up to 3×, and persisted in
SQLite — an ACK'd `succeeded` means *the board executed it* (it does not prove
the PC booted).

## Security notes

- Token auth everywhere; keep the token file `600`, never commit it.
- `sdkconfig` (generated at build) contains your credentials — it's gitignored;
  only `sdkconfig.defaults` placeholders are committed.
- TLS terminates at your reverse proxy; relayd itself is loopback-only.

## License

MIT — see [LICENSE](LICENSE).

---

## 中文速览

十几块钱的 ESP32-C3 Super Mini + 任意一台有 HTTPS 的服务器，就能做出互联网远程开机：
固件长轮询自建 relayd 队列，收到 `WOL` 就发魔术包唤醒电脑，`PULSE` 则用 GPIO3 短接
主板开机排针 400ms（当物理开机键用）。

**重点踩坑**：Super Mini 连不上 WiFi（能扫到 AP、认证阶段超时 reason=2），不是密码/
加密方式问题，是**发射功率太高**——把 `esp_wifi_set_max_tx_power` 从默认约 20dBm
降到 **15dBm**（配置值 `60`）即可秒杀，本项目 `sdkconfig.defaults` 已内置该修复。
