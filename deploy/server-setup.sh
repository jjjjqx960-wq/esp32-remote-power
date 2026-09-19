#!/usr/bin/env bash
# Idempotent installer for relayd (ESP32 remote power command relay).
# Safe to re-run. Tested targets: Debian/Ubuntu with systemd.
#
# Usage:  sudo bash deploy/server-setup.sh
# Env overrides: RELAYD_PORT (59777), RELAYD_USER (root), RELAYD_HOME (/opt/relayd)
set -euo pipefail

PORT="${RELAYD_PORT:-59777}"
HOME_DIR="${RELAYD_HOME:-/opt/relayd}"
TOKEN_DIR=/etc/relayd
TOKEN_FILE=$TOKEN_DIR/token
SERVICE=/etc/systemd/system/relayd.service
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root (sudo bash $0)" >&2; exit 1
fi

# 1. layout + token
install -d -m 700 "$TOKEN_DIR" "$HOME_DIR"
if [ ! -s "$TOKEN_FILE" ]; then
    openssl rand -hex 20 > "$TOKEN_FILE"
    chmod 600 "$TOKEN_FILE"
    echo "generated new token -> $TOKEN_FILE"
fi

# 2. daemon
if [ -f "$SRC_DIR/server/relayd.py" ]; then
    install -m 644 "$SRC_DIR/server/relayd.py" "$HOME_DIR/relayd.py"
elif command -v curl >/dev/null; then
    curl -fsSL "https://raw.githubusercontent.com/jjjjqx960-wq/esp32-remote-power/main/server/relayd.py" \
        -o "$HOME_DIR/relayd.py"
else
    echo "no relayd.py found and curl missing" >&2; exit 1
fi

# 3. systemd unit
cat > "$SERVICE" <<EOF
[Unit]
Description=ESP32 command relay (relayd)
After=network.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 $HOME_DIR/relayd.py \\
    --host 127.0.0.1 --port $PORT \\
    --token-file $TOKEN_FILE \\
    --state-file $HOME_DIR/state.sqlite3
Restart=always
RestartSec=3
NoNewPrivileges=true
ProtectSystem=strict
ReadWritePaths=$HOME_DIR
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now relayd
sleep 1

# 4. smoke test
if curl -fsS "http://127.0.0.1:$PORT/status?t=$(cat "$TOKEN_FILE")" | grep -q '"ok":true'; then
    echo "relayd is running on 127.0.0.1:$PORT"
    echo "token: $(cat "$TOKEN_FILE")"
    echo "next: put an HTTPS reverse proxy in front (see docs/DEPLOYMENT.md A3)"
else
    echo "relayd failed smoke test; check: journalctl -u relayd -n 30" >&2; exit 1
fi
