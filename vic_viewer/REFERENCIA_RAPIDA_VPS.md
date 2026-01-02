# 🔧 TABLA DE REFERENCIA RÁPIDA - VicViewer VPS

## 📊 SERVICIOS VICVIEWER - UBICACIONES EXACTAS

| Servicio | Puerto | Usuario | Directorio | Archivo Systemd | Estado |
|----------|---------|---------|------------|-----------------|---------|
| **Matchmaker** | 8787 | vicvmk | `/opt/vic_viewer-matchmaker/` | `/etc/systemd/system/vic-viewer-matchmaker.service` | ✅ ACTIVO |
| **Túnel** | 9400/9401 | root | `/root/vic_tunnel/` | `/etc/systemd/system/vic-tunnel.service` | ✅ ACTIVO |
| **TURN** | 3479 | root | `/opt/vic_viewer-turn/` | `/etc/systemd/system/vic-viewer-turn.service` | ✅ ACTIVO |

## 🚨 SERVICIOS DE OTROS PROYECTOS (NO TOCAR)

| Servicio | Puerto | Proyecto | Estado | ⚠️ Acción |
|----------|---------|----------|---------|-----------|
| controlremoto-matchmaker | 8080 | ControlRemoto | Activo | ❌ NO MODIFICAR |
| cr-matchmaker | - | ControlRemoto | Activo | ❌ NO MODIFICAR |

## 🔍 COMANDOS DE VERIFICACIÓN RÁPIDA

```bash
# Conectar al VPS
ssh root@38.242.234.197

# Ver SOLO servicios VicViewer
systemctl status vic-viewer-matchmaker vic-tunnel vic-viewer-turn

# Ver puertos VicViewer
ss -tlnp | grep -E "(8787|9400|9401|3479)"

# Logs de errores recientes
journalctl -u vic-viewer-matchmaker --since "1 hour ago" --no-pager
journalctl -u vic-tunnel --since "1 hour ago" --no-pager
journalctl -u vic-viewer-turn --since "1 hour ago" --no-pager
```

## 🌐 ENDPOINTS DE PRUEBA

| Servicio | URL/Comando de Prueba | Respuesta Esperada |
|----------|----------------------|-------------------|
| Matchmaker | `curl http://38.242.234.197:8787/health` | `{"status":"ok","activeSessions":0,"ttlMs":120000}` |
| Túnel Control | `telnet 38.242.234.197 9400` | Conexión exitosa |
| TURN | `telnet 38.242.234.197 3479` | Conexión exitosa |

## 📁 ARCHIVOS IMPORTANTES

### Configuraciones Systemd
```
/etc/systemd/system/vic-viewer-matchmaker.service
/etc/systemd/system/vic-tunnel.service
/etc/systemd/system/vic-viewer-turn.service
```

### Códigos Fuente
```
/opt/vic_viewer-matchmaker/dist/server.js       # Matchmaker compilado
/root/vic_tunnel/server.js                      # Túnel relay
```

### Logs del Sistema
```
journalctl -u vic-viewer-matchmaker
journalctl -u vic-tunnel
journalctl -u vic-viewer-turn
```

## 🛠️ OPERACIONES COMUNES

### Reiniciar TODOS los servicios VicViewer
```bash
systemctl restart vic-viewer-matchmaker vic-tunnel vic-viewer-turn
systemctl status vic-viewer-matchmaker vic-tunnel vic-viewer-turn
```

### Verificar que TODO esté funcionando
```bash
# Desde el VPS
ss -tlnp | grep -E "(8787|9400|9401|3479)" | wc -l
# Debe devolver: 4 (un puerto por servicio)
```

### Desde Cliente Windows
```powershell
cd c:\vic_viewer
.\test_connectivity.ps1
# Debe mostrar ✅ en todos los servicios
```

---

**💡 REGLA DE ORO**: 
- ✅ Servicios con prefijo `vic-viewer-` o `vic-tunnel` → SON NUESTROS
- ❌ Servicios con `controlremoto` o `cr-` → NO SON NUESTROS (NO TOCAR)

**🔗 VPS**: 38.242.234.197 | **Usuario**: root | **Fecha**: 7 octubre 2025