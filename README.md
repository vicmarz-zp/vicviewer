# ControlRemoto

ControlRemoto es un sistema de escritorio remoto distribuido como binarios portables para Windows. Utiliza WebRTC para conexión P2P de baja latencia y ofrece perfiles de red optimizados para diferentes escenarios (LAN, Internet, Móvil).

## 🆕 Novedades v1.3 - Perfiles de Red Optimizados

**Nueva funcionalidad:** Sistema de perfiles que optimiza automáticamente según el tipo de conexión.

### Uso rápido en LAN
```powershell
# Host
.\bin\cr-host.exe -profile lan

# Viewer
.\bin\cr-viewer.exe -profile lan
```

**Perfil LAN incluye:**
- ✅ Bitrate 3.5 Mbps (3x mayor que default)
- ✅ 24 FPS (33% más fluido)
- ✅ Resolución Full HD (1920px)
- ✅ Latencia 80-120ms típica

Ver [PERFILES_LAN_QUICKSTART.md](PERFILES_LAN_QUICKSTART.md) para guía completa.

## Arquitectura a alto nivel

- **UI (`pkg/ui`)**: Ventanas nativas Win32 con un diseño mínimo (fondo #F5F8FA y acentos índigo #3B82F6) que muestran/consumen el ID y la URL de señalización, con canvas interactivo para capturar eventos.
- **Capa de aplicación (`pkg/app`)**: Orquesta el ciclo de vida; genera y muestra el ID, inicia el servidor de señalización y el gestor WebRTC.
- **Señalización (`pkg/signaling`)**: Implementación embebida HTTP para pruebas locales/LAN y conector `matchmaker` para un servicio externo que expone la oferta SDP y acepta la respuesta del visor.
- **Transporte (`pkg/webrtc`)**: Administra el `PeerConnection` de Pion WebRTC, canal de datos para eventos de control y canal de vídeo (VP8 o JPEG encapsulado en data channel).
- **Captura (`pkg/capture`)**: Proveedor de pantalla Win32 que captura, redimensiona y comprime frames JPEG para el canal de datos.
- **Control (`pkg/control`)**: Contratos para inyectar eventos de mouse/teclado en Windows.
- **Utilidades (`pkg/session`, `internal/logging`)**: Generación de ID legible y logger base.
- **Visor (`pkg/viewer`, `pkg/render`, `pkg/ui/viewer_windows.go`)**: Cliente P2P que negocia WebRTC, renderiza frames JPEG entrantes y reenvía eventos locales (mouse/teclado) por el canal de control.

## Conectividad (matchmaker + TURN)

- **VPS oficial**: la distribución del proyecto asume el matchmaker desplegado en `http://38.242.234.197:8080`. Antes de lanzar los binarios en un equipo nuevo, exporta `CONTROLREMOTO_MATCHMAKER_URL=http://38.242.234.197:8080` (o su variante HTTPS) o configúralo desde la UI del host.
- **Archivo TURN**: copia `config/turn.json` junto a los ejecutables o en `%APPDATA%\ControlRemoto\turn.json`. El archivo ya contiene las URLs y credenciales del coturn instalado en el mismo VPS.
- **Fallback local**: si el entorno no puede alcanzar el VPS, siempre puedes arrancar el host con `-signaling-mode=local` para pruebas en LAN.

## Flujo de conexión (visión MVP)

1. El host genera un ID aleatorio y arranca el relay HTTP local.
2. El host crea una oferta WebRTC y la publica en el relay bajo ese ID.
3. El visor obtiene la oferta mediante el ID, negocia WebRTC (vía STUN) y sube una respuesta.
4. Con la respuesta, ambos establecen la sesión P2P (o vía TURN/relay externo en futuras iteraciones).
5. El host envía frames por la pista de vídeo o por el canal de datos; el visor envía eventos de control por el canal `control`.

> Limitaciones actuales del visor: falta sincronización avanzada (frame pacing adaptativo) y soporte en la variante `walkui`; la inyección de teclado depende de la disposición local del host remoto.

## Túneles WireGuard dinámicos

Para entornos donde WebRTC no es viable (firewalls estrictos, QoS cerrado o necesidad de rutas dedicadas) se añadió un overlay WireGuard dinámico:

- **Servicio `tunnel-broker`** (`cmd/tunnel-broker`): API REST que genera sesiones efímeras con claves, IPs y snippets para el concentrador.
- **Librería `pkg/tunnel`**: puede reutilizarse desde otros componentes (matchmaker, backend) para automatizar la entrega de credenciales.
- **Script PowerShell** (`scripts/request-wireguard-session.ps1`): cliente que crea sesiones y guarda archivos `.conf` listos para `wireguard.exe`.
- **Guía operativa**: ver `docs/DYNAMIC_TUNNELING.md` para despliegue paso a paso.

El flujo recomendado es solicitar un túnel, aplicar las configuraciones en host/viewer y anunciar la IP overlay a la lógica de señalización/control para transportar vídeo y eventos con latencia controlada.

## Requisitos

- Windows 10/11 (x86_64).
- [Go 1.21+](https://go.dev/dl/) con `gcc` disponible para compilar dependencias que usan Win32/CGO.

## Compilación rápida

```powershell
cd c:\ControlRemoto
go mod tidy
go build -ldflags "-H=windowsgui" -o bin\cr-host.exe cmd\cr-host
go build -ldflags "-H=windowsgui" -o bin\cr-viewer.exe cmd\cr-viewer
go build -o bin\cr-matchmaker.exe cmd\cr-matchmaker
go build -ldflags "-H=windowsgui" -o bin\cr-control.exe cmd\controlremoto
```

## Compilación con soporte VP8 (experimental)

> Requisitos adicionales: `libvpx` y `pkg-config` disponibles desde el toolchain mingw-w64 de MSYS2.

1. Instala el toolchain y `libvpx` desde MSYS2 y expón las rutas necesarias en tu sesión de PowerShell:

    ```powershell
    # Opción rápida: script automatizado
    powershell -ExecutionPolicy Bypass -File .\scripts\setup-vp8-toolchain.ps1

    # Opción manual (si prefieres hacerlo paso a paso)
    # Instala MSYS2 si aún no lo tienes: https://www.msys2.org/
    C:\msys64\usr\bin\bash -lc "pacman --noconfirm -Syu"
    C:\msys64\usr\bin\bash -lc "pacman --noconfirm -S mingw-w64-x86_64-gcc mingw-w64-x86_64-pkg-config mingw-w64-x86_64-libvpx"

    setx MSYS2_ROOT C:\msys64
    setx PATH "%PATH%;C:\\msys64\\mingw64\\bin"
    setx PKG_CONFIG_PATH C:\msys64\mingw64\lib\pkgconfig
    ```

    > Si previamente instalaste `libvpx` con vcpkg/MSVC, asegúrate de quitar esas rutas del `PATH`/`PKG_CONFIG_PATH` para evitar conflictos de enlazado con símbolos como `__security_cookie`.

2. (Opcional) Verifica que `pkg-config` resuelva correctamente las banderas:

    ```powershell
    $env:PKG_CONFIG_PATH = "C:/msys64/mingw64/lib/pkgconfig"
    pkg-config --libs --cflags vpx
    ```

3. Compila con la etiqueta `vp8` para habilitar el nuevo pipeline.

    ```powershell
    go build -tags vp8 -ldflags "-H=windowsgui" -o bin\cr-host.exe cmd\cr-host
    go build -tags vp8 -ldflags "-H=windowsgui" -o bin\cr-viewer.exe cmd\controlremoto-viewer
    ```

    Los scripts incluidos aceptan un switch para aplicar esta etiqueta automáticamente:

    ```powershell
    powershell -ExecutionPolicy Bypass -File .\build.ps1 -EnableVP8
    powershell -ExecutionPolicy Bypass -File .\build-debug.ps1 -EnableVP8
    powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1 -EnableVP8
    powershell -ExecutionPolicy Bypass -File .\scripts\run-demo.ps1 -EnableVP8
    ```

4. Ejecuta las pruebas que dependen de VP8 usando el mismo tag (asegúrate de exportar `PKG_CONFIG_PATH` en la consola actual):

    ```powershell
    $env:PKG_CONFIG_PATH = "C:/msys64/mingw64/lib/pkgconfig"
    $env:CGO_ENABLED = "1"
    go test -tags vp8 ./...
    ```

## Guía rápida para usar los binarios

Los paquetes generados con `scripts\build-release.ps1` incluyen `cr-host.exe`, `cr-viewer.exe`, `cr-matchmaker.exe` y un `LEEME.txt` con instrucciones para el cliente final. Resumen:

### Selector compacto (cr-control.exe)

- Ejecuta `cr-control.exe` cuando quieras una sola ventana muy ligera con pestañas **Ver PC** / **Mostrar PC**.
- Elige el modo desde la pestaña correspondiente y pulsa **Abrir** para lanzar el visor o el host tradicionales.
- Al cerrar el modo elegido, el selector vuelve a mostrarse para que cambies de rol sin reiniciar la aplicación.
- Puedes arrancar directamente un modo específico con `cr-control.exe -mode host` o `cr-control.exe -mode viewer` si no necesitas la ventana compacta.

### 1. Servicio matchmaker (opcional pero recomendado)

- Ejecuta `cr-matchmaker.exe` en un equipo accesible por host y visor (LAN o Internet).
- Ejemplo de ejecución:

    ```powershell
    cr-matchmaker.exe -addr :8081 -session-ttl 15m -cleanup 1m
    ```

- Si lo expones públicamente, protégelo detrás de HTTPS y restringe el acceso.
- Los scripts (`run-demo.ps1`, `build-release.ps1`) y el visor usan por defecto `http://38.242.234.197:8080` como URL del matchmaker. Ajusta las variables de entorno `CONTROLREMOTO_MATCHMAKER_URL` y `CONTROLREMOTO_SIGNALING_URL` si necesitas otro endpoint.

### 2. Equipo remoto (host)

- Ejecuta `cr-host.exe` en el equipo que compartirá la pantalla (doble clic, sin abrir consola negra).
- El host usa por defecto el servicio matchmaker fijo (`http://38.242.234.197:8080`); si necesitas otro endpoint, ajusta la URL desde el menú o exporta `CONTROLREMOTO_MATCHMAKER_URL` antes de abrirlo.
- La ventana muestra un código de 9 dígitos y copia la invitación al portapapeles. Cuando el estado cambia a **Conectado**, la ventana pasa a la bandeja del sistema y el tooltip del icono resume código + códec activo (MJPEG/VP8).
- Para alcanzar redes NAT estrictas configura un servidor TURN propio (ver `docs/TURN_SETUP.md`). Simplemente deja un `turn.json` junto al ejecutable (o en `config/turn.json`, `%APPDATA%\ControlRemoto\turn.json` o `%ProgramData%\ControlRemoto\turn.json`) y la app lo cargará automáticamente. Si necesitas rutas personalizadas, puedes seguir usando `CR_TURN_CONFIG` o el script `scripts/set-turn-env.ps1`.
- Desde el botón **Opciones** puedes cambiar la URL del matchmaker, elegir la calidad de transmisión (Equilibrada/Alta) y fijar un código permanente que no caduca para futuras sesiones. El host regenera la invitación al guardar: la calidad se ajusta al instante, el código se actualiza sin reiniciar y la nueva invitación se copia automáticamente al portapapeles.

### 3. Equipo local (visor)

- Ejecuta `cr-viewer.exe` en el PC que controlará remotamente (doble clic, sin consola adicional).
- Usa **Pegar invitación** para rellenar código y URL, revisa los datos y pulsa **Conectar**.
- Ajusta el buffer y el intervalo de render desde el panel avanzado si la red es inestable.
- El botón **Copiar estado** replica el último mensaje mostrado (incluyendo errores) para compartirlo rápidamente.

### 4. Prueba rápida en la misma máquina

- Ejecuta `run-demo.ps1` para lanzar host y visor desde un solo equipo:

    ```powershell
    powershell -ExecutionPolicy Bypass -File .\scripts\run-demo.ps1
    ```

- Flags útiles: `-SkipBuild` si ya compilaste, `-NoViewer` para omitir el visor.


## Emparejamiento remoto con servicio Matchmaker

El binario `cr-matchmaker.exe` levanta un microservicio HTTP que gestiona códigos numéricos de 9 dígitos para emparejar un host y un visor sin exponer direcciones IP. El flujo completo queda así:

1. Ejecutas el servicio `matchmaker` en una ubicación accesible por ambos equipos (LAN o Internet con un túnel HTTPS).
2. El host arranca en modo `matchmaker`, registra una sesión y publica su oferta WebRTC usando solo el código corto que muestra la UI.
3. El visor introduce el mismo código; la app recupera la oferta desde el servicio `matchmaker`, envía su respuesta y establece el `PeerConnection`.

### Endpoints expuestos

- `POST /v1/sessions`: crea o reserva un código de sesión. Si no incluyes `code`, el servicio genera uno de 9 dígitos.
- `PUT/GET /v1/sessions/{code}/offer`: aloja la oferta SDP del host.
- `PUT/GET /v1/sessions/{code}/answer`: aloja la respuesta SDP del visor.
- `GET /healthz`: verificación simple para monitoreo.

Los códigos expiran automáticamente (`SessionTTL`, por defecto 15 minutos). El barrido periódico (`CleanupInterval`) limpia sesiones caducadas.

### Puesta en marcha para pruebas reales

#### 1. Compila los binarios

Usa los comandos anteriores o ejecuta `scripts\build-release.ps1` para obtener `cr-host.exe`, `cr-viewer.exe` y `cr-matchmaker.exe` en la carpeta `bin` (o en `dist\...` si usas el script).

#### 2. Despliega el servicio matchmaker

```powershell
cd c:\ControlRemoto
bin\cr-matchmaker.exe -addr ":8081" -session-ttl 15m -cleanup 1m
```

> Si expones el servicio en Internet, publícalo detrás de un proxy HTTPS y considera un túnel con autenticación básica.

#### 3. Arranca el host en modo remoto

```powershell
# O bien exporta la variable para no repetir la URL
$env:CONTROLREMOTO_MATCHMAKER_URL = "https://tu-dominio.example.com"

bin\cr-host.exe -signaling-mode matchmaker -matchmaker-url https://tu-dominio.example.com -session-code-length 9
```

La ventana del host mostrará el código generado; compártelo con el visor.

#### 4. Conecta el visor

Inicia `cr-viewer.exe`, pega el código en la UI y asegúrate de que el campo URL apunta al servicio matchmaker (`https://tu-dominio.example.com`).

Cuando ambos extremos estén conectados, verás el flujo de vídeo / control bidireccional. Si necesitas volver al modo LAN, ejecuta el host con `-signaling-mode local` (o deja la configuración por defecto).

## Interfaz renovada y bandeja del sistema

- **Paleta**: fondo claro #F5F8FA para reducir el contraste, texto primario #17253D y acentos índigo #3B82F6. Se complementa con verde #10B981 para estados satisfactorios y ámbar #F97316 para advertencias.
- **Host minimalista**: el código de 9 dígitos se muestra en grande y legible, con botones directos para copiar código, URL o invitación. Los mensajes de estado usan colores suaves según el contexto.
- **Ocultamiento automático**: en cuanto la sesión WebRTC pasa a `Connected`, la ventana del host se oculta y queda como icono en la bandeja del sistema (área de iconos ocultos). Desde ahí puedes hacer doble clic para restaurar o clic derecho para abrir un menú con “Mostrar ventana” y “Cerrar”. El tooltip del icono muestra el código vigente y el códec activo para confirmarlo sin abrir la ventana.
- **Conectividad TURN embebida (relay-only por defecto)**: El binario incluye una configuración TURN embebida y ahora opera en modo `ForceRelay` por defecto (solo candidatos relay). Esto garantiza conectividad estable en NAT estrictos sin necesidad de definir variables de entorno. Si deseas realizar pruebas usando también candidatos host/srflx, inicia con el flag `-force-relay=false` (host y/o viewer) o establece `CR_FORCE_RELAY=false` (en futuras versiones). Puedes seguir sobrescribiendo la configuración dejando un `turn.json` junto al ejecutable o definiendo `CR_TURN_CONFIG` / `CR_TURN_URLS` + `CR_TURN_USERNAME` + `CR_TURN_PASSWORD`.
- **Reconexión guiada**: si la conexión se interrumpe, la ventana vuelve a primer plano y el estado cambia a tono ámbar para indicar que debes revisar la red o reiniciar la sesión.
- **Reoferta automática**: cuando el visor pierde la conexión, el host reinicia la sesión WebRTC y publica una nueva oferta tras unos segundos, permitiendo que el cliente se reconecte sin intervención manual.
- **Supervisión en tiempo real**: la ventana de sesión ahora muestra un indicador lateral con FPS suavizados, latencia extremo a extremo, profundidad de cola y marcos descartados para diagnosticar la experiencia sin recurrir a herramientas externas.
- **Descartes inteligentes**: el visor purga los cuadros intermedios cuando detecta backlog en la cola para mostrar siempre el fotograma más reciente y evitar saltos largos al recuperar fluidez. Desde la ventana de sesión puedes desactivar esta optimización (modo "fidelidad") si prefieres conservar todos los cuadros para revisar cada cambio.
- **Reintentos automáticos**: si el visor pierde la conexión WebRTC intenta reconectarse de forma incremental (backoff exponencial hasta 15 s), informando en la UI cada intento y conservando la sesión abierta mientras el host siga disponible.

### Instalación / distribución

- Usa `scripts\build-release.ps1` para generar `dist\controlremoto-YYYYMMDD-HHMMSS` con los tres binarios, `run-demo.ps1` y un `LEEME.txt` listo para el cliente.
- Distribuye el binario `cr-matchmaker.exe` solo en el entorno donde vayas a alojar el servicio (puede correrse como servicio de Windows o dentro de un contenedor minimalista en Linux con `systemd`).
- Comparte `cr-host.exe` con los equipos que compartirán su pantalla; `cr-viewer.exe` queda para los revisores.

> Recomendación: si publicas el servicio en Internet, agrega primero autenticación básica o restringe IPs con tu firewall para evitar que terceros abusen de los códigos.

## Script de demostración

```powershell
cd c:\ControlRemoto
powershell -ExecutionPolicy Bypass -File .\scripts\run-demo.ps1
```

El script compila (a menos que uses `-SkipBuild`) y lanza host y visor en ventanas separadas; copia el ID y la URL que muestra el host hacia el visor para iniciar la sesión. Puedes omitir el visor con `-NoViewer` o saltar la compilación con `-SkipBuild`. Los parámetros de captura/render se pueden ajustar al vuelo:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-demo.ps1 -CaptureWidth 1400 -JPEGQuality 80 -ViewerBufferSize 5
```

Argumentos disponibles:

- `-CaptureFPS <int>`: cuadros por segundo objetivo del host.
- `-CaptureWidth <int>`: ancho máximo antes de reescalar la captura.
- `-JPEGQuality <int>`: calidad JPEG (1-100).
- `-ViewerBufferSize <int>`: tamaño de la cola de frames en el visor.
- `-ViewerFrameInterval <int>`: intervalo mínimo entre renders en milisegundos.

### Paquete para compartir con otro equipo

```powershell
cd c:\ControlRemoto
powershell -ExecutionPolicy Bypass -File .\scripts\build-release.ps1
```

El script genera `dist\controlremoto-YYYYMMDD-HHMMSS` con:

- `cr-host.exe`: ejecutar en el equipo remoto que comparte pantalla.
- `cr-viewer.exe`: ejecutar en tu equipo local para visualizar/controlar.
- `cr-matchmaker.exe`: desplegar donde alojes el servicio de emparejamiento.
- `cr-control.exe`: selector compacto para iniciar host o visor desde una sola app.
- `run-demo.ps1`: útil si quieres lanzar ambos desde un mismo equipo.
- `LEEME.txt`: instrucciones rápidas para el usuario final.

Comparte la carpeta completa con el equipo remoto (o comprímela en `.zip`).

> Nota: El build alternativo `walkui` aún compila, pero no implementa el renderizado JPEG por data channel.

## Parámetros de rendimiento

- **Host (`cmd/controlremoto-server`)**: expone flags `-capture-fps`, `-capture-width` y `-jpeg-quality` para balancear nitidez y uso de CPU/bitrate sin recompilar.
- **Visor (`cmd/controlremoto-viewer`)**: admite `-buffer-size` y `-frame-interval` (ms) para controlar la cola de cuadros y el pacing del render.
- El visor descarta automáticamente los cuadros rezagados cuando acumula backlog para priorizar la imagen más reciente y reducir la latencia visual percibida; puedes alternar esta función desde la UI (modo fluido vs. modo fidelidad).
- El visor reintenta la negociación con el host si WebRTC se cae, aplicando backoff exponencial (1s → 2s → 4s… hasta 15s) y manteniendo informados a quienes observan desde la ventana de sesión.

### Bitrate adaptativo y telemetría

El host ajusta dinámicamente el bitrate VP8 ante fallos de publicación consecutivos y se recupera gradualmente cuando la transmisión se estabiliza. Todos los eventos se registran en logs (`metrics host bitrate_event`) y también se envían al visor mediante un canal de datos dedicado (`telemetry`). El visor refleja el bitrate actual en sus snapshots de métricas (`metrics viewer snapshot`).

Flags del host relevantes:

| Flag | Descripción | Default |
|------|-------------|---------|
| `-vp8-bitrate` | Bitrate inicial objetivo (bps) | 2000000 |
| `-vp8-min-bitrate` | Límite inferior permitido (bps) | 300000 |
| `-vp8-down-consecutive` | Errores consecutivos de publicación para disparar reducción | 6 |
| `-vp8-down-factor` | Factor multiplicador al reducir (0.75 = -25%) | 0.75 |
| `-vp8-recovery-interval-ms` | Intervalo sin errores antes de intentar subir | 5000 |
| `-vp8-recovery-factor` | Factor multiplicador de recuperación (1.10 = +10%) | 1.10 |
| `-telemetry-label` | Nombre del data channel de telemetría (vacío = desactivar) | telemetry |

Razones de eventos de bitrate:

- `initial`: bitrate inicial aplicado.
- `errores_consecutivos`: reducción tras umbral de fallos.
- `recuperacion`: incremento gradual hacia el objetivo base.

Los scripts `scripts\run-diagnostics.ps1` y `scripts\run-diagnostics-matrix.ps1` extraen: bitrate inicial/final, número de eventos y bitrate actual visto por el visor.

### Panel y endpoints de métricas locales

El host expone (loopback) un micro-servidor de inspección con:

| Endpoint | Descripción |
|----------|-------------|
| `GET /metrics/last` | Último snapshot JSON (estado adaptativo + thresholds) |
| `GET /metrics/stream` | Flujo SSE (event: snapshot) cada ~1s |
| `GET /metrics/history` | Buffer circular (snapshots recientes) |
| `GET /metrics/ui` | Panel HTML ligero sin dependencias externas |

Contenido del panel `/metrics/ui`:

- Estado adaptativo (stable / pressure / congestion / recovering / recovered)
- Bitrate actual vs base + sparkline 60s
- Presión (barra + sparkline) combinando latencia, cola y error rate
- Métricas del visor (latencia ms, profundidad de cola, bitrate)
- Intentos y errores de publicación + tasa agregada (% error)
- Contadores (bitrate_events, adaptation_events, downgrades)
- JSON crudo para copiar

Variables de entorno:

| Variable | Efecto | Default |
|----------|--------|---------|
| `CR_METRICS_HISTORY_CAP` | Tamaño del buffer en memoria | 300 |
| `CR_DISABLE_METRICS_UI` | Oculta el panel `/metrics/ui` si se define | (vacío) |
| `CR_METRICS_TOKEN` | Token requerido (Bearer o ?token=) para endpoints | (vacío) |

Ejemplo:

```powershell
$env:CR_METRICS_TOKEN = "secreto123"
$env:CR_METRICS_HISTORY_CAP = "900"
bin\cr-host.exe
```

Consumir SSE autenticado:

```powershell
curl -H "Authorization: Bearer secreto123" http://127.0.0.1:PORT/metrics/stream
```

O en navegador: `http://127.0.0.1:PORT/metrics/ui?token=secreto123`.

Fórmula de presión:

```text
pressure = 0.5*lat_norm + 0.3*queue_norm + 0.2*error_rate
```

Umbrales de referencia:

- > 0.85 congestión / riesgo alto
- 0.60 – 0.85 presión moderada
- < 0.60 estable

Recuperación sólo cuando latencia y cola están bajo sus umbrales “low” y el error rate < 10%.

### Telemetría inversa (viewer → host)

El visor envía cada snapshot (500 ms) un objeto JSON por el canal `vtelemetry` con:
`{"type":"viewer_metrics","fps":...,"latency_ms":...,"queue":"d/c","dropped":N,"bitrate_bps":B}`.
El host lo registra como `metrics host viewer_metrics` y además lo persiste en `logs/metrics.jsonl` junto a los eventos de bitrate.

### Reintentos y reconexión mejorados

La reconexión ahora aplica backoff con jitter (25%) y clasifica:

- Errores de señalización (oferta no encontrada): incrementos suaves.
- Desconexión tras conexión estable: reinicia a una ventana moderada.
- Fallos WebRTC (failed): escalada más rápida hasta el tope.
Tras 10s estables se restablece el backoff base.

## Preferencias persistentes del host

- Las elecciones del botón **Opciones** se guardan en `%AppData%\ControlRemoto\host\preferences.json`, por lo que la URL del matchmaker, la calidad de transmisión y el estado del código permanente sobreviven entre ejecuciones.
- La calidad **Alta** (valor por defecto) captura a resolución completa con JPEG 92 y un buffer de 5 cuadros para mejorar nitidez; puedes volver al modo **Equilibrado** si necesitas reducir ancho de banda. Cada vez que guardas, la invitación se vuelve a emitir con el código y la URL vigentes, sin necesidad de reiniciar el host.
- Al habilitar el código permanente, el host reutiliza el mismo ID de 9 dígitos hasta que vuelvas a desmarcar la opción; ideal para escenarios de soporte recurrente o kioscos.

## Flujo del visor

1. El usuario abre `cr-viewer.exe`, configura la URL de señalización si es necesario, ingresa el ID recibido del host y presiona **Conectar**.
2. El visor consulta el endpoint de señalización configurado (por defecto `http://127.0.0.1:8080`) y obtiene la oferta WebRTC.
3. Se genera una respuesta SDP, se publica al host y se establece el `PeerConnection`.
4. Los cuadros llegan codificados en JPEG por un canal de datos y se pintan en el canvas Win32; el canvas captura eventos de mouse/teclado y los envía por el canal `control`.

> Limitaciones actuales del visor: falta soporte para múltiples monitores y un pipeline alternativo para `walkui`.

## Próximos pasos sugeridos

- Integrar un proveedor de captura (DXGI Desktop Duplication o GDI) que emita VP8.
- Inyectar eventos de mouse/teclado mediante `SendInput` (Windows API).
- Sustituir el relay local por un servicio público con TLS y TURN.
- Empaquetado y firma del binario (Squirrel, WiX, o zip simple) y auto-actualización.
- Telemetría básica y registro de sesiones.

