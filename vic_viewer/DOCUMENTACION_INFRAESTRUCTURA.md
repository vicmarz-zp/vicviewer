# 📋 DOCUMENTACIÓN OFICIAL - INFRAESTRUCTURA VicViewer

**Fecha de creación**: 7 de octubre de 2025  
**Estado**: OPERATIVO Y VERIFICADO ✅  
**VPS**: 38.242.234.197 (Contabo Ubuntu 24.04.3 LTS)

---

## 🌍 UBICACIÓN DE SERVICIOS EN VPS

### VPS: 38.242.234.197
**Sistema Operativo**: Ubuntu 24.04.3 LTS  
**Proveedor**: Contabo  
**Acceso SSH**: `ssh root@38.242.234.197`

---

## 📂 ESTRUCTURA DE DIRECTORIOS

```
/opt/
├── vic_viewer-matchmaker/          # Servicio Matchmaker VicViewer
│   ├── dist/server.js             # Código compilado Node.js
│   ├── src/server.ts              # Código fuente TypeScript
│   ├── package.json               # Dependencias Node.js
│   └── node_modules/              # Módulos instalados
│
├── vic_viewer-turn/               # Servicio TURN VicViewer
│   └── (configuración coturn)
│
├── vic_viewer-tunnel/             # Servicio Túnel VicViewer (deprecated)
│   └── (archivos antiguos)
│
└── controlremoto/                 # PROYECTO DIFERENTE
    └── (servicios ControlRemoto - NO TOCAR)

/root/
└── vic_tunnel/                    # Servicio Túnel VicViewer ACTIVO
    ├── server.js                  # Servidor relay Node.js
    ├── package.json               # Dependencias
    └── node_modules/              # Módulos uuid
```

---

## 🔧 SERVICIOS SYSTEMD INSTALADOS

### VicViewer Services (NUESTRO PROYECTO)

#### 1. vic-viewer-matchmaker.service
- **Archivo**: `/etc/systemd/system/vic-viewer-matchmaker.service`
- **Directorio**: `/opt/vic_viewer-matchmaker/`
- **Puerto**: 8787
- **Usuario**: vicvmk
- **Estado**: ✅ ACTIVO
- **Comando Start**: `systemctl start vic-viewer-matchmaker`
- **Logs**: `journalctl -u vic-viewer-matchmaker`

#### 2. vic-tunnel.service  
- **Archivo**: `/etc/systemd/system/vic-tunnel.service`
- **Directorio**: `/root/vic_tunnel/`
- **Puertos**: 9400 (control), 9401 (data)
- **Usuario**: root
- **Estado**: ✅ ACTIVO
- **Comando Start**: `systemctl start vic-tunnel`
- **Logs**: `journalctl -u vic-tunnel`

#### 3. vic-viewer-turn.service
- **Archivo**: `/etc/systemd/system/vic-viewer-turn.service`
- **Puerto**: 3479 (TCP/UDP)
- **Estado**: ✅ ACTIVO
- **Comando Start**: `systemctl start vic-viewer-turn`
- **Logs**: `journalctl -u vic-viewer-turn`

### Otros Services (PROYECTOS DIFERENTES - NO TOCAR)

#### controlremoto-matchmaker.service
- **Puerto**: 8080
- **Proyecto**: ControlRemoto (DIFERENTE)
- **Estado**: Activo (NO relacionado con VicViewer)

#### cr-matchmaker.service
- **Proyecto**: ControlRemoto (DIFERENTE)  
- **Estado**: Activo (NO relacionado con VicViewer)

---

## 🌐 PUERTOS Y CONECTIVIDAD

### VicViewer Ports (NUESTROS)
- **8787**: Matchmaker HTTP/WebSocket
- **9400**: Túnel Control TCP
- **9401**: Túnel Data TCP  
- **3479**: TURN Server TCP/UDP

### Otros Proyectos (NO NUESTROS)
- **8080**: ControlRemoto Matchmaker
- **80/443**: Nginx (general)
- **22**: SSH

### Firewall Status
```bash
# Verificar puertos abiertos
ufw status | grep -E "(8787|9400|9401|3479)"
```

---

## 📋 COMANDOS DE ADMINISTRACIÓN

### Verificar Estado de TODOS los Servicios VicViewer
```bash
# Conectar al VPS
ssh root@38.242.234.197

# Verificar servicios VicViewer
systemctl status vic-viewer-matchmaker
systemctl status vic-tunnel  
systemctl status vic-viewer-turn

# Ver logs en tiempo real
journalctl -u vic-viewer-matchmaker -f
journalctl -u vic-tunnel -f
journalctl -u vic-viewer-turn -f
```

### Reiniciar Servicios (si es necesario)
```bash
# Reiniciar Matchmaker
systemctl restart vic-viewer-matchmaker

# Reiniciar Túnel
systemctl restart vic-tunnel

# Reiniciar TURN
systemctl restart vic-viewer-turn
```

### Verificar Conectividad desde Cliente
```powershell
# Desde Windows cliente
cd c:\vic_viewer
.\test_connectivity.ps1
```

---

## 🔍 SOLUCIÓN DE PROBLEMAS

### Si un servicio no responde:

1. **Verificar estado**:
   ```bash
   systemctl status [nombre-servicio]
   ```

2. **Ver logs de error**:
   ```bash
   journalctl -u [nombre-servicio] --lines=50
   ```

3. **Reiniciar servicio**:
   ```bash
   systemctl restart [nombre-servicio]
   ```

4. **Verificar puertos**:
   ```bash
   ss -tlnp | grep [puerto]
   ```

### Endpoints de Health Check

- **Matchmaker**: `http://38.242.234.197:8787/health`
- **Túnel Control**: `telnet 38.242.234.197 9400`
- **TURN**: `telnet 38.242.234.197 3479`

---

## ⚠️ IMPORTANTE - SEPARACIÓN DE PROYECTOS

### VicViewer Services (NUESTROS):
- `vic-viewer-matchmaker.service` ✅
- `vic-tunnel.service` ✅  
- `vic-viewer-turn.service` ✅

### ControlRemoto Services (OTROS - NO TOCAR):
- `controlremoto-matchmaker.service` ❌ NO TOCAR
- `cr-matchmaker.service` ❌ NO TOCAR

**NUNCA modificar o detener servicios que NO sean de VicViewer**

---

## 📊 VERIFICACIÓN COMPLETA

### Script de Prueba Automática
```powershell
# Ejecutar desde c:\vic_viewer\
.\test_connectivity.ps1

# Resultado esperado:
# ✅ Matchmaker Status: CONNECTED
# ✅ TURN Server TCP: REACHABLE  
# ✅ TURN Server UDP: CLIENT CONNECTED
# ✅ Tunnel Control TCP: REACHABLE
```

### Estado Operativo Actual
- **Matchmaker**: ✅ FUNCIONANDO (puerto 8787)
- **Túnel Relay**: ✅ FUNCIONANDO (puertos 9400/9401)
- **TURN Server**: ✅ FUNCIONANDO (puerto 3479)
- **Conectividad**: ✅ VERIFICADA desde cliente

---

## 📞 CONTACTOS DE EMERGENCIA

**VPS Provider**: Contabo (support@contabo.com)  
**IP VPS**: 38.242.234.197  
**Usuario SSH**: root

---

**✅ ESTADO FINAL**: Todos los servicios VicViewer están instalados, configurados y funcionando correctamente en el VPS 38.242.234.197.

*Documentación actualizada: 7 de octubre de 2025*