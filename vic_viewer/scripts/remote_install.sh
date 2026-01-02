#!/usr/bin/env bash
# Script de instalación remota del matchmaker VicViewer
# Uso: ./remote_install.sh ubuntu@38.242.234.197

set -euo pipefail

if [[ $# -eq 0 ]]; then
    echo "Uso: $0 usuario@servidor [--secret SECRET]"
    echo "Ejemplo: $0 ubuntu@38.242.234.197 --secret mi_secreto_opcional"
    exit 1
fi

SERVER="$1"
SECRET=""

shift
while [[ $# -gt 0 ]]; do
    case "$1" in
        --secret)
            SECRET="$2"
            shift 2
            ;;
        *)
            echo "Opción desconocida: $1" >&2
            exit 1
            ;;
    esac
done

echo "=== Instalando VicViewer Matchmaker en $SERVER ==="

# 1. Copiar archivos al servidor
echo "1. Copiando archivos..."
rsync -av --delete --exclude=node_modules --exclude=dist matchmaker/ "$SERVER:/tmp/vicviewer-matchmaker/"

# 2. Ejecutar instalación remota
echo "2. Ejecutando instalación remota..."
INSTALL_CMD="
set -euo pipefail
cd /tmp/vicviewer-matchmaker

# Instalar Node.js si no existe
if ! command -v node >/dev/null 2>&1; then
    curl -fsSL https://deb.nodesource.com/setup_20.x | sudo bash -
    sudo apt-get install -y nodejs
fi

# Instalar dependencias del sistema
sudo apt-get update -y
sudo apt-get install -y git ufw

# Crear directorio de aplicación
sudo mkdir -p /opt/vicviewer-matchmaker
sudo rsync -a --delete ./ /opt/vicviewer-matchmaker/

cd /opt/vicviewer-matchmaker

# Instalar dependencias NPM y compilar
npm install
npm run build

# Crear usuario de servicio
if ! id -u vicvmk >/dev/null 2>&1; then
    sudo useradd -r -s /usr/sbin/nologin vicvmk
fi
sudo chown -R vicvmk:vicvmk /opt/vicviewer-matchmaker

# Crear archivo de servicio systemd
sudo tee /etc/systemd/system/vicviewer-matchmaker.service > /dev/null <<EOF
[Unit]
Description=VicViewer Matchmaker Service
After=network.target

[Service]
Type=simple
User=vicvmk
WorkingDirectory=/opt/vicviewer-matchmaker
Environment=PORT=8787
Environment=SESSION_TTL_MS=120000
$([ -n \"$SECRET\" ] && echo \"Environment=MATCHMAKER_SECRET=$SECRET\")
ExecStart=/usr/bin/node dist/server.js
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

# Habilitar y iniciar servicio
sudo systemctl daemon-reload
sudo systemctl enable vicviewer-matchmaker
sudo systemctl restart vicviewer-matchmaker

# Configurar firewall
sudo ufw allow 8787/tcp || true
sudo ufw --force enable || true

echo '=== Instalación completada ==='
sudo systemctl status vicviewer-matchmaker --no-pager
echo ''
echo 'Probar con: curl http://38.242.234.197:8787/health'
"

ssh "$SERVER" "$INSTALL_CMD"

echo ""
echo "=== Instalación completada ==="
echo "Probando conexión..."
sleep 3
curl -f http://38.242.234.197:8787/health || echo "Nota: El servicio puede tardar unos segundos en estar listo"