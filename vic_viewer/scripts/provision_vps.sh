#!/usr/bin/env bash
set -euo pipefail
# Provisiona el VPS completo: dependencias del sistema, despliegue inicial del matchmaker y endurecimiento básico.
# Uso: sudo ./provision_vps.sh --secret "CLAVE_OPCIONAL" --port 8787

PORT=8787
SECRET=""
MATCHMAKER_DIR="/opt/vicviewer-matchmaker"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)
      PORT="$2"; shift 2 ;;
    --secret)
      SECRET="$2"; shift 2 ;;
    *) echo "Arg desconocido: $1" >&2; exit 1 ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "Ejecutar con sudo" >&2
  exit 1
fi

apt-get update -y
apt-get install -y curl git ufw rsync

if ! command -v node >/dev/null 2>&1; then
  curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
  apt-get install -y nodejs
fi

# Copiar matchmaker si el repo ya está en /tmp/source (sugiere rsync previo)
if [[ -d /tmp/source/matchmaker ]]; then
  mkdir -p "$MATCHMAKER_DIR"
  rsync -a --delete /tmp/source/matchmaker/ "$MATCHMAKER_DIR"/
fi

pushd "$MATCHMAKER_DIR" >/dev/null || { echo "Directorio matchmaker no encontrado" >&2; exit 1; }

npm install
npm run build

USER_SVC="vicvmk"
if ! id -u "$USER_SVC" >/dev/null 2>&1; then
  useradd -r -s /usr/sbin/nologin "$USER_SVC"
fi
chown -R "$USER_SVC":"$USER_SVC" "$MATCHMAKER_DIR"

cat >/etc/systemd/system/vicviewer-matchmaker.service <<EOF
[Unit]
Description=VicViewer Matchmaker
After=network.target

[Service]
Type=simple
User=$USER_SVC
WorkingDirectory=$MATCHMAKER_DIR
Environment=PORT=$PORT
EOF

if [[ -n "$SECRET" ]]; then
  echo "Environment=MATCHMAKER_SECRET=$SECRET" >> /etc/systemd/system/vicviewer-matchmaker.service
fi

cat >>/etc/systemd/system/vicviewer-matchmaker.service <<'EOF'
ExecStart=/usr/bin/node dist/server.js
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable vicviewer-matchmaker --now

ufw allow $PORT/tcp || true
ufw --force enable || true

systemctl status --no-pager vicviewer-matchmaker
popd >/dev/null

echo "Provision completo. Health: curl http://$(curl -s ifconfig.me):$PORT/health"
