# NVENC Hardware Encoding - VicViewer

## ¿Qué es NVENC?

NVENC (NVIDIA Video Encoder) es el encoder de video por hardware de las GPUs NVIDIA. Permite encoding H.264/HEVC **10-20x más rápido** que software, con latencia ultra-baja ideal para streaming remoto.

## Rendimiento Comparativo

| Resolución | VP8 Software | NVENC H.264 | Mejora |
|------------|-------------|-------------|--------|
| 1080p | 26 FPS / 38ms | 120+ FPS / <8ms | **~5x** |
| 720p | 56 FPS / 18ms | 200+ FPS / <5ms | **~4x** |
| 4K | ~8 FPS | 60+ FPS | **~8x** |

## Requisitos

### Hardware
- GPU NVIDIA con NVENC:
  - GeForce GTX 600+ (Kepler) - encoding básico
  - GeForce GTX 900+ (Maxwell) - HEVC
  - GeForce GTX 1000+ (Pascal) - encoding avanzado
  - GeForce RTX 2000+ (Turing) - encoding premium
  - GeForce RTX 3000/4000 (Ampere/Ada) - AV1

### Software
- Driver NVIDIA **450.0+** (recomendado: 535+)
- Windows 10/11 64-bit

## Verificar Disponibilidad

Ejecuta el test de NVENC:

```powershell
.\build\tests\Release\vic_nvenc_test.exe
```

Salida esperada si NVENC está disponible:
```
=== NVENC Detection Test ===
Checking NVENC availability...
NVENC: Max supported API version: 12.2
NVENC: API loaded successfully
NVENC: Available (H.264 hardware encoding)
```

## Uso Automático

VicViewer **detecta automáticamente** si NVENC está disponible:

1. Al iniciar el host, intenta crear encoder NVENC
2. Si NVENC no está disponible, hace fallback a VP8 software
3. No requiere configuración manual

```cpp
// El código usa createBestEncoder() que auto-selecciona:
auto encoder = vic::encoder::createBestEncoder();
// Retorna NvencEncoder si hay GPU NVIDIA, sino Vp8Encoder
```

## Configuración Avanzada

Para ajustar parámetros NVENC manualmente:

```cpp
#include "NvencEncoder.h"

vic::encoder::NvencConfig config;
config.targetBitrateKbps = 8000;   // 8 Mbps
config.maxBitrateKbps = 12000;     // 12 Mbps máximo
config.gopLength = 60;              // Keyframe cada 60 frames
config.lowLatencyMode = true;       // Ultra-low latency
config.useBFrames = false;          // Sin B-frames (menor latencia)
config.useHEVC = false;             // H.264 (mejor compatibilidad)

auto encoder = vic::encoder::createNvencEncoder(config);
```

## Troubleshooting

### "nvEncodeAPI DLL not found"

1. Verifica driver NVIDIA instalado:
   ```powershell
   nvidia-smi
   ```

2. Actualiza driver desde [nvidia.com/drivers](https://www.nvidia.com/drivers)

3. Verifica que existe el DLL:
   ```powershell
   Test-Path "C:\Windows\System32\nvEncodeAPI64.dll"
   ```

### "NVENC session open failed"

- Posible límite de sesiones simultáneas (GeForce: 3 sesiones)
- Cierra otras apps que usen NVENC (OBS, Discord, etc.)

### "Unsupported GPU"

- GPU muy antigua sin NVENC
- GPU de laptop con NVENC deshabilitado
- Verifica en [developer.nvidia.com/video-encode-decode-gpu-support-matrix](https://developer.nvidia.com/video-encode-decode-gpu-support-matrix)

## Comparación VP8 vs H.264 (NVENC)

| Aspecto | VP8 (libvpx) | H.264 (NVENC) |
|---------|--------------|---------------|
| CPU Usage | 100% (software) | <5% (hardware) |
| Latencia | 17-40ms | 2-8ms |
| Calidad | Buena | Excelente |
| Bitrate | Mayor | Menor (mejor compresión) |
| Compatibilidad | WebRTC nativo | Requiere decoder H.264 |
| GPUs soportadas | Todas | Solo NVIDIA |

## API Reference

```cpp
namespace vic::encoder {

// Verificar si NVENC está disponible
bool isNvencAvailable();

// Obtener info del encoder
std::string getNvencInfo();

// Crear encoder NVENC específico
std::unique_ptr<VideoEncoder> createNvencEncoder(
    const NvencConfig& config, 
    ID3D11Device* device = nullptr
);

// Crear mejor encoder disponible (NVENC o VP8)
std::unique_ptr<VideoEncoder> createBestEncoder();

}
```

## Notas de Implementación

- Formato de entrada: BGRA → NV12 (conversión automática)
- Formato de salida: H.264 Annex B (NAL units)
- Rate control: CBR Low-Delay HQ
- Preset: P1 (ultra-low latency) o P4 (balanced)
- No usa B-frames para latencia mínima
