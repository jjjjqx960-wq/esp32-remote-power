# ESP32-C3 Super Mini: Wi-Fi connects only after lowering TX power

A write-up of the nastiest bug in this project, published so the next person
doesn't spend days on it.

## Symptom

- Board: ESP32-C3 Super Mini (and likely other boards with cramped RF layout /
  tiny PCB antenna)
- `esp_wifi_scan` sees the AP perfectly: channel 1, RSSI ≈ −58 dBm, mesh with
  two BSSIDs
- `esp_wifi_connect()` always fails ~4 s later:

```
wifi:state: init -> auth (b0)
wifi:start 4000ms AUTH timer
wifi:auth timeout -> disconnected, reason: auth expire(2)
```

  (MicroPython surfaces the same thing as status `202`.)

## Everything that was ruled out (so don't re-test these)

| Hypothesis | Result |
|---|---|
| Wrong password | identical trace with right/wrong password; wrong passwords die at 4-way handshake (`reason 15`), we never got that far |
| WPA2/WPA3 transition mode | six-config matrix on native IDF — force WPA2-PSK, force WPA3-SAE+H2E, PMF on/off, all zero-config, 11b/g-only — **all identical** |
| PMF / 802.11w | zero PMF/SAE lines in driver logs; PMF is negotiated at *assoc*, we fail at *auth* |
| MicroPython zeroing the config struct | native IDF with the same zeroed struct fails identically |
| 5 GHz vs 2.4 GHz | separate issue; ESP32-C3 is 2.4 GHz-only, but the failure above is on a 2.4 GHz AP |
| AP-side crypto misconfiguration | RSN IE sniffed: `group CCMP / pairwise CCMP / AKM=[PSK,SAE] / caps=0x008C` — MFPC=1 MFPR=0, normal |

The driver log shows the auth frame is sent and **the AP simply never replies** —
on both mesh BSSIDs. The board can *hear* the AP (beacons, scan results), but
the AP apparently can't hear the board.

## Root cause

**The STA's transmit power is too high for the board's RF path.** These ultra-cheap
boards have lossy antenna matching / poor layout; at the ESP-IDF default max TX
power (~20 dBm, `esp_wifi_set_max_tx_power(80)`) the uplink signal is effectively
unusable — distorted/overdriven at the PA or desensitizing the receive chain
enough that the AP ignores the auth frame (or the response gets mangled).

## Fix

```c
esp_wifi_set_max_tx_power(60);   // units are 0.25 dBm → 60 = 15 dBm
```

Result: immediate connect, `GOT_IP`, stable. (An earlier pass at 40 = 10 dBm
also worked; 15 dBm keeps more margin.)

In this firmware it's the Kconfig option `CONFIG_RELAY_WIFI_TX_POWER`
(default `60`). If your board still fails at 15 dBm, try `40` (10 dBm).

## How to reproduce the diagnostic

1. Flash any minimal STA firmware that prints disconnect reasons.
2. Watch for `init -> auth` → `auth timeout` with `reason 2` (never reaching
   `auth -> assoc`).
3. Bake a SoftAP into the same firmware (`ESP32C3-TXTEST`, open): if a phone can
   see *and join* it, the board's RX/TX both work at short range — pointing
   even harder at TX-power-vs-real-AP rather than dead hardware.
4. Lower `esp_wifi_set_max_tx_power()` stepwise (80 → 60 → 40) and watch the
   failure disappear.

## Notes

- `esp_wifi_set_max_tx_power` takes units of **0.25 dBm** (80 = 20 dBm, 60 = 15,
  40 = 10). Easy to misread as raw dBm.
- This is a *board* problem, not an ESP32-C3 problem — full-size devkits with
  proper antennas connect fine at default power.
- Lower TX power slightly reduces range but fixes integrity; on a same-room or
  same-home use case it is strictly a win.
