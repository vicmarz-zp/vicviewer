#!/bin/bash
# Verificar y arrancar matchmaker en VPS
# Ejecutar desde: ssh root@38.242.234.197

echo "=== Diagnóstico de Matchmaker VPS ==="

# 1. Verificar si el servicio está corriendo
echo "1. Estado del servicio matchmaker:"
systemctl status vicviewer-matchmaker || echo "Servicio no encontrado"

# 2. Verificar puerto 8787
echo -e "\n2. Puertos en escucha:"
netstat -tlnp | grep 8787 || echo "Puerto 8787 no en escucha"

# 3. Verificar logs recientes
echo -e "\n3. Logs recientes del matchmaker:"
journalctl -u vicviewer-matchmaker --lines=20 --no-pager || echo "No hay logs del servicio"

# 4. Verificar si existe el directorio y archivos
echo -e "\n4. Archivos del matchmaker:"
ls -la /opt/vicviewer-matchmaker/ 2>/dev/null || echo "Directorio no existe"

# 5. Verificar procesos node.js
echo -e "\n5. Procesos Node.js activos:"
ps aux | grep node | grep -v grep || echo "No hay procesos Node.js"

# 6. Intentar arrancar el servicio
echo -e "\n6. Intentando arrancar servicio..."
systemctl start vicviewer-matchmaker
systemctl enable vicviewer-matchmaker
sleep 3

# 7. Verificar estado final
echo -e "\n7. Estado final:"
systemctl status vicviewer-matchmaker --no-pager
netstat -tlnp | grep 8787 || echo "Puerto 8787 aún no disponible"

echo -e "\n=== Fin diagnóstico ==="