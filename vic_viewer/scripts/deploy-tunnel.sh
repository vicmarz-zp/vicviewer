#!/usr/bin/env bash
set -euo pipefail

CONTROL_PORT=9400
DATA_PORT=9401
APP_DIR="/opt/vicviewer-tunnel"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --control)
      CONTROL_PORT="$2"
      shift 2
      ;;
    --data)
      DATA_PORT="$2"
      shift 2
      ;;
    --app-dir)
      APP_DIR="$2"
      shift 2
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "This script must be run with sudo." >&2
  exit 1
fi

OWNER="${SUDO_USER:-root}"

if ! command -v node >/dev/null; then
  curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
  apt-get install -y nodejs
fi

apt-get install -y git rsync

mkdir -p "$APP_DIR"
rsync -a --delete "$REPO_ROOT/tunnel/" "$APP_DIR/"
cd "$APP_DIR"

chown -R "$OWNER":"$OWNER" "$APP_DIR"

sudo -u "$OWNER" npm install

cat >/etc/systemd/system/vicviewer-tunnel.service <<SERVICE
[Unit]
Description=VicViewer Tunnel Server
After=network.target

[Service]
Type=simple
User=$OWNER
WorkingDirectory=$APP_DIR
Environment=VIC_TUNNEL_CONTROL_PORT=$CONTROL_PORT
Environment=VIC_TUNNEL_DATA_PORT=$DATA_PORT
ExecStart=/usr/bin/npm start
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
SERVICE

systemctl daemon-reload
systemctl enable vicviewer-tunnel
systemctl restart vicviewer-tunnel
systemctl status --no-pager vicviewer-tunnel
