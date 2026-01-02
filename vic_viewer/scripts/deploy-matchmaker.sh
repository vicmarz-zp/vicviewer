#!/usr/bin/env bash
set -euo pipefail

PORT=8787
SECRET=""
APP_DIR="/opt/vicviewer-matchmaker"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT="$2"
      shift 2
      ;;
    --secret)
      SECRET="$2"
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

if ! command -v curl >/dev/null; then
  apt-get update
  apt-get install -y curl
fi

if ! command -v node >/dev/null; then
  curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
  apt-get install -y nodejs
fi

apt-get install -y git rsync

mkdir -p "$APP_DIR"
rsync -a --delete "$REPO_ROOT/matchmaker/" "$APP_DIR/"
cd "$APP_DIR"

chown -R "$OWNER":"$OWNER" "$APP_DIR"

sudo -u "$OWNER" npm install
sudo -u "$OWNER" npm run build

cat >/etc/systemd/system/vicviewer-matchmaker.service <<SERVICE
[Unit]
Description=VicViewer Matchmaker
After=network.target

[Service]
Type=simple
User=$OWNER
WorkingDirectory=$APP_DIR
Environment=PORT=$PORT
SERVICE

if [[ -n "$SECRET" ]]; then
  echo "Environment=MATCHMAKER_SECRET=$SECRET" >> /etc/systemd/system/vicviewer-matchmaker.service
fi

cat >>/etc/systemd/system/vicviewer-matchmaker.service <<'SERVICE'
ExecStart=/usr/bin/npm start
Restart=on-failure

[Install]
WantedBy=multi-user.target
SERVICE

systemctl daemon-reload
systemctl enable vicviewer-matchmaker
systemctl restart vicviewer-matchmaker
systemctl status --no-pager vicviewer-matchmaker
