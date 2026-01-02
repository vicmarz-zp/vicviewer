#!/bin/bash
# Script para arrancar matchmaker en VPS
# Uso: ssh root@38.242.234.197 'bash -s' < restart_matchmaker.sh

echo "=== Arrancando Matchmaker en VPS ==="

# Verificar si el servicio systemd existe
if systemctl is-enabled vicviewer-matchmaker >/dev/null 2>&1; then
    echo "Usando servicio systemd..."
    systemctl stop vicviewer-matchmaker 2>/dev/null
    systemctl start vicviewer-matchmaker
    systemctl enable vicviewer-matchmaker
    sleep 3
    systemctl status vicviewer-matchmaker --no-pager
else
    echo "Servicio systemd no encontrado, arrancando manualmente..."
    
    # Buscar directorio del matchmaker
    if [ -d "/opt/vicviewer-matchmaker" ]; then
        cd /opt/vicviewer-matchmaker
    elif [ -d "/home/matchmaker" ]; then
        cd /home/matchmaker
    elif [ -d "/root/vicviewer-matchmaker" ]; then
        cd /root/vicviewer-matchmaker
    else
        echo "ERROR: No se encontró el directorio del matchmaker"
        exit 1
    fi
    
    # Matar procesos existentes
    pkill -f "node.*server.js" 2>/dev/null || true
    
    # Arrancar en background
    nohup node server.js > matchmaker.log 2>&1 &
    echo "Matchmaker arrancado con PID: $!"
    sleep 2
fi

# Verificar que está funcionando
echo -e "\nVerificando puerto 8787..."
if netstat -tlnp | grep -q ":8787 "; then
    echo "✅ Puerto 8787 está en escucha"
else
    echo "❌ Puerto 8787 NO está en escucha"
fi

echo -e "\nProcesos node activos:"
ps aux | grep node | grep -v grep || echo "No hay procesos node"

echo "=== Fin ==="