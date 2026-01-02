#!/usr/bin/env bash
set -euo pipefail
# Instala dependencias y configura el servicio systemd en el VPS
# Uso: ./install_matchmaker_vps.sh (se asume Ubuntu y usuario con sudo)

sudo apt-get update -y
sudo apt-get install -y curl git ufw
if ! command -v node >/dev/null 2>&1; then
  curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash -
  sudo apt-get install -y nodejs
fi

APP_DIR=/opt/vicviewer-matchmaker
sudo mkdir -p "$APP_DIR"
sudo rsync -a --delete ./ "$APP_DIR"/
(cd "$APP_DIR" && sudo npm install && sudo npm run build)

# Crear usuario de sistema opcional
if ! id -u vicvmk >/dev/null 2>&1; then
  sudo useradd -r -s /usr/sbin/nologin vicvmk || true
fi
sudo chown -R vicvmk:vicvmk "$APP_DIR"

SERVICE_FILE=/etc/systemd/system/vicviewer-matchmaker.service
sudo bash -c "cat > $SERVICE_FILE" <<'EOF'
[Unit]
Description=VicViewer Matchmaker Service
After=network.target

[Service]
Type=simple
User=vicvmk
WorkingDirectory=/opt/vicviewer-matchmaker
Environment=PORT=8787
Environment=SESSION_TTL_MS=120000
ExecStart=/usr/bin/node dist/server.js
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable vicviewer-matchmaker.service --now

sudo ufw allow 8787/tcp || true

echo "Matchmaker desplegado y activo en puerto 8787"
systemctl status vicviewer-matchmaker.service --no-pager
