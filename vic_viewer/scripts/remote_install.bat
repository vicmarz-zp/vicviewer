@echo off
REM Script de instalación remota para Windows
REM Requiere WSL, Git Bash o PowerShell con OpenSSH

echo === Instalando VicViewer Matchmaker en el VPS ===

if "%1"=="" (
    echo Uso: %0 usuario@servidor [SECRET]
    echo Ejemplo: %0 ubuntu@38.242.234.197 mi_secreto_opcional
    exit /b 1
)

set SERVER=%1
set SECRET=%2

echo 1. Copiando archivos al servidor...
scp -r matchmaker %SERVER%:/tmp/vicviewer-matchmaker

echo 2. Ejecutando instalación remota...
ssh %SERVER% "
    set -euo pipefail
    cd /tmp/vicviewer-matchmaker

    # Instalar Node.js si no existe
    if ! command -v node >/dev/null 2>&1; then
        curl -fsSL https://deb.nodesource.com/setup_20.x | sudo bash -
        sudo apt-get install -y nodejs
    fi

    # Dependencias del sistema
    sudo apt-get update -y
    sudo apt-get install -y git ufw

    # Crear directorio de aplicación
    sudo mkdir -p /opt/vicviewer-matchmaker
    sudo rsync -a --delete ./ /opt/vicviewer-matchmaker/
    cd /opt/vicviewer-matchmaker

    # Instalar y compilar
    npm install
    npm run build

    # Usuario de servicio
    if ! id -u vicvmk >/dev/null 2>&1; then
        sudo useradd -r -s /usr/sbin/nologin vicvmk
    fi
    sudo chown -R vicvmk:vicvmk /opt/vicviewer-matchmaker

    # Servicio systemd
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
$(if [ -n '%SECRET%' ]; then echo 'Environment=MATCHMAKER_SECRET=%SECRET%'; fi)
ExecStart=/usr/bin/node dist/server.js
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

    # Habilitar servicio
    sudo systemctl daemon-reload
    sudo systemctl enable vicviewer-matchmaker
    sudo systemctl restart vicviewer-matchmaker

    # Firewall
    sudo ufw allow 8787/tcp || true
    sudo ufw --force enable || true

    echo === Instalación completada ===
    sudo systemctl status vicviewer-matchmaker --no-pager
"

echo.
echo === Probando conexión ===
timeout 3 >nul 2>&1
curl -f http://38.242.234.197:8787/health || echo Nota: El servicio puede tardar unos segundos

echo.
echo Instalación completada. El matchmaker está corriendo en http://38.242.234.197:8787