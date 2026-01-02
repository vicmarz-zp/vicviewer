# VicViewer

VicViewer is a desktop screen sharing prototype for Windows with remote tunneling capabilities for cross-network connectivity.

## 📋 DOCUMENTACIÓN CRÍTICA

- **[📋 Infraestructura VPS](DOCUMENTACION_INFRAESTRUCTURA.md)** - Ubicación exacta de TODOS los servicios
- **[🔧 Referencia Rápida VPS](REFERENCIA_RAPIDA_VPS.md)** - Comandos y verificaciones esenciales
- **[✅ Estado Final](ESTADO_FINAL_TUNNELES.md)** - Resumen completo de implementación

## Components

- `vicviewer/` – C++ desktop application with host/viewer UI
- `matchmaker/` – Node.js service that brokers connection codes between peers (optional)
- `tunnel/` – Node.js relay that creates dynamic TCP tunnels between host and viewer across different networks
- **Remote Infrastructure** – VPS-hosted services enable connectivity between different IP addresses and networks

## Prerequisites (Windows)

- Visual Studio 2022 with C++ Desktop Development workload or MSVC build tools
- CMake 3.24+
- Ninja or MSBuild generator
- Node.js 18+
- libvpx development headers/libraries (install via vcpkg or your package manager)

## Installing libvpx

### Windows (vcpkg)

1. Install [vcpkg](https://learn.microsoft.com/vcpkg/get_started/overview) and set the `VCPKG_ROOT` environment variable.
2. Install the port matching your target architecture:

    ```powershell
    vcpkg install libvpx:x86-windows
    # or for 64-bit builds
    vcpkg install libvpx:x64-windows
    ```

3. When configuring with CMake provide the vcpkg toolchain (the bootstrap script does this automatically when `VCPKG_ROOT` is detected):

    ```powershell
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x86-windows
    ```

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build libvpx-dev nodejs npm
```

## Quick start using scripts

```powershell
# Configure and build (Win32 by default)
./scripts/bootstrap.ps1

# Launch the C++ desktop app (rebuilds if needed)
./scripts/run-vicviewer.ps1

# Start the matchmaker service on port 8787
./scripts/run-matchmaker.ps1 -Install

# (Opcional) Definir variables del túnel y lanzar la app
$env:VIC_RELAY_HOST = "38.242.234.197"
$env:VIC_RELAY_CONTROL_PORT = "9400"
$env:VIC_RELAY_DATA_PORT = "9401"
./launch_test.ps1 -Mode host
```

> To generate 64-bit binaries, append `-Architecture x64` to `bootstrap.ps1` and `run-vicviewer.ps1`. The scripted bootstrap automatically calls the appropriate `vcvars*.bat` profile if it exists.

## Building manually

```powershell
cd vicviewer
cmake -S . -B build -G Ninja -A Win32
cmake --build build
```

## Running unit tests

```powershell
cmake --build build --target vic_unit_tests
ctest --test-dir build --output-on-failure
```

## Remote Infrastructure Status (VPS 38.242.234.197)

✅ **Matchmaker Service**: Active on port 8787 for session brokering
✅ **Tunnel Relay Service**: Active on ports 9400 (control) and 9401 (data)  
✅ **TURN Server**: Active on port 3479 for NAT traversal

### Service Status Check

```powershell
# Test connectivity to all remote services
./test_connectivity.ps1
```

**Current Status**: All services are running and accessible ✅

## Running the matchmaker manually

```powershell
cd matchmaker
npm install
npm run build
PORT=8787 npm start
```

## Deploying the tunnel relay on Ubuntu (38.242.234.197)

### Tunnel Relay Deployment Status

El servicio de túnel está desplegado y activo en el VPS.

The tunnel relay is currently running as a systemd service. To check status:

```bash
ssh root@38.242.234.197 "systemctl status vic-tunnel"
```

To redeploy or update:

1. Copy the repository to the VPS (for example, `/opt/vicviewer`).
2. SSH into the server and run:

    ```bash
    cd /opt/vicviewer/scripts
    sudo ./deploy-tunnel.sh --control 9400 --data 9401
    ```

    The service installs Node.js if required, syncs the `tunnel/` directory to `/opt/vicviewer-tunnel`, installs dependencies and registers the `vicviewer-tunnel` systemd unit.
3. Manage the service with:

    ```bash
    sudo systemctl status vicviewer-tunnel
    sudo journalctl -u vicviewer-tunnel -f
    ```

Clients discover the relay automatically unless you set `VIC_DISABLE_TUNNEL=1`. Override the host or ports with:

```powershell
$env:VIC_RELAY_HOST = "mi-servidor"
$env:VIC_RELAY_CONTROL_PORT = "9440"
$env:VIC_RELAY_DATA_PORT = "9441"
```

### Provisioning desde cero (script integral)

Si el servidor está limpio, puedes copiar el directorio del repo (o sólo `matchmaker/`) a `/tmp/source` y luego ejecutar:

```bash
cd /opt/vicviewer/scripts
sudo ./provision_vps.sh --port 8787 --secret "<shared-secret>"
```

El script:

- Instala Node.js 20, git, ufw
- Crea usuario de sistema `vicvmk`
- Copia `matchmaker/` a `/opt/vicviewer-matchmaker`
- Instala dependencias y compila TypeScript
- Configura y habilita el servicio systemd
- Abre el puerto 8787/tcp en ufw

Luego comprueba:

```bash
curl http://38.242.234.197:8787/health
```

## Usage Overview

1. Start the matchmaker (`npm run dev`).
2. Launch `VicViewer.exe`.
3. In the **Compartir pantalla** tab, click **Iniciar host** to obtain a code. The window minimizes to the tray.
4. On the viewer machine, open VicViewer, switch to **Ver pantalla**, enter the code, and press **Conectar**.
5. Viewer receives the live desktop stream and can interact with the host machine.

## Recent enhancements

- VP8 encoding/decoding now relies on libvpx for significantly better compression quality.
- Heartbeat support in the transport layer detects dropped links and triggers automatic cleanup.
- Matchmaker enforces TTL-based session expiry, optional shared secret authentication, and exposes a `/health` endpoint.
- Helper scripts (`bootstrap`, `run-vicviewer`, `run-matchmaker`, `deploy-matchmaker`) streamline builds, testing, and deployment.
- Unit tests are registered with CTest, making `ctest --output-on-failure` usable out of the box.
