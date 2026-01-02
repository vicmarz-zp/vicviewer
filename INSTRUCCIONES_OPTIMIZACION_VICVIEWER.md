# 📋 Instrucciones para Optimizar VicViewer


---

## PROMPT PARA OPTIMIZACIÓN DE VICVIEWER

```
Eres un experto en optimización de aplicaciones de video en tiempo real para Windows. 
Voy a pedirte que optimices este proyecto de escritorio remoto (VicViewer) para alcanzar 
rendimiento comparable a aplicaciones comerciales como Parsec o Moonlight.

## CONTEXTO DEL PROYECTO
- Aplicación de escritorio remoto nativa C++ para Windows
- Arquitectura modular: capture → encoder → transport → decoder → render
- Actualmente usa: DXGI capture, VP8 software encoding, GDI rendering
- Latencia actual: 100-300ms
- Objetivo: <50ms de latencia, 60fps estables

## PROBLEMAS IDENTIFICADOS (PRIORIDAD DE IMPACTO)

### 🔴 CRÍTICO 1: VP8 Software Encoding
- Ubicación: encoder/ (SimpleVp8Encoder.cpp)
- Problema: libvpx consume 50-80% CPU por frame
- Solución: Implementar NVENC (NVIDIA) como encoder principal, con fallback a VP8

### 🔴 CRÍTICO 2: Conversión de color en CPU
- Ubicación: SimpleVp8Encoder.cpp líneas 128-145
- Problema: Loop BGRA→I420 pixel por pixel = O(n) muy lento
- Solución: Usar libyuv de Google (SIMD optimizado) o hacer conversión en GPU shader

### 🔴 CRÍTICO 3: Rendering con GDI
- Ubicación: VicViewerUI.cpp (StretchDIBits)
- Problema: Sin aceleración GPU, escalado en CPU
- Solución: Direct3D 11 con texturas, o al menos DirectDraw

### 🟡 IMPORTANTE 4: Sleeps innecesarios
- Ubicación: HostSession.cpp
- Problema: Sleep(50), Sleep(16), Sleep(100) añaden latencia artificial
- Solución: Usar eventos/condition_variables, capturar tan rápido como DXGI provea frames

### 🟡 IMPORTANTE 5: Decodificación YUV→BGRA en CPU  
- Ubicación: SimpleVp8Decoder.cpp líneas 104-123
- Solución: DXVA2 para decode en GPU, o al menos libyuv con SIMD

## PLAN DE IMPLEMENTACIÓN (EN ORDEN)

### FASE 1: Quick Wins (1-2 días)
1. Eliminar todos los Sleep() y reemplazar por event-driven
2. Subir VP8 bitrate: 2000 → 4000-6000 kbps
3. Cambiar kDefaultCpuUsed de 4 → 8 (faster encoding, less quality)
4. Agregar flag para desactivar sleeps de debug

### FASE 2: SIMD Color Conversion (3-5 días)
1. Integrar libyuv: https://chromium.googlesource.com/libyuv/libyuv/
2. Reemplazar el loop BGRA→I420 por libyuv::ARGBToI420()
3. Reemplazar I420→BGRA por libyuv::I420ToARGB()
4. Ganancia esperada: 5-10x más rápido

### FASE 3: NVENC Integration (1-2 semanas) ⭐ MÁXIMO IMPACTO
1. Descargar NVIDIA Video Codec SDK
2. Crear NvencEncoder clase que implemente la misma interfaz que SimpleVp8Encoder
3. Recibir ID3D11Texture2D directamente de DXGI capture (sin copia a RAM)
4. Configurar NVENC para low-latency preset:
   ```cpp
   NV_ENC_INITIALIZE_PARAMS initParams = {};
   initParams.encodeConfig->rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ;
   initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
   initParams.encodeConfig->frameIntervalP = 1; // No B-frames
   ```
5. Output H.264/HEVC en lugar de VP8
6. Fallback a VP8 si no hay GPU NVIDIA

### FASE 4: Direct3D Rendering (1 semana)
1. Crear D3D11 device y swap chain en viewer
2. Crear shader simple para YUV→RGB (o usar DXVA2)
3. Renderizar frame como textura con DrawIndexed()
4. Eliminar completamente GDI/StretchDIBits

### FASE 5: Zero-Copy Pipeline (avanzado)
1. DXGI captura → ID3D11Texture2D (ya en GPU)
2. NVENC encode directamente de texture (sin staging)
3. Network transfer (único momento fuera de GPU)
4. DXVA2 decode → ID3D11Texture2D
5. D3D11 render directo de texture

## MÉTRICAS DE ÉXITO
- [ ] Latencia glass-to-glass < 50ms
- [ ] 60 FPS estables sin drops
- [ ] CPU usage < 15% en host
- [ ] CPU usage < 10% en viewer  
- [ ] GPU usage < 30% (encoding)
- [ ] Funciona en hardware NVIDIA GTX 1000+

## CÓDIGO DE REFERENCIA

### Para NVENC, referirse a:
- NVIDIA Video Codec SDK samples: AppEncD3D11
- OBS Studio: obs-studio/plugins/obs-nvenc/

### Para libyuv:
```cpp
#include "libyuv.h"
libyuv::ARGBToI420(
    src_argb, src_stride_argb,
    dst_y, dst_stride_y,
    dst_u, dst_stride_u,
    dst_v, dst_stride_v,
    width, height);
```

### Para D3D11 rendering:
- DirectXTK SimpleSample
- Microsoft DirectX samples en GitHub

## RESTRICCIONES
- Mantener compatibilidad con Windows 10+
- No romper el fallback a software (VP8) para PCs sin GPU compatible
- Mantener la arquitectura modular existente
- El viewer debe poder conectarse a hosts con diferentes encoders

## CUANDO TERMINES CADA FASE
1. Medir latencia con herramienta de timestamp
2. Medir FPS real (no el configurado)
3. Medir CPU/GPU usage
4. Comparar antes/después
5. Documentar qué cambió

¿Entendido? Empieza analizando el código actual del encoder y proponiendo 
la integración de NVENC como primera optimización mayor.
```

---

## 📝 Tips Adicionales para la Otra Ventana

1. **Empieza pidiendo que analice el encoder actual**:
   > "Analiza SimpleVp8Encoder.cpp y muéstrame exactamente qué cambiar para integrar NVENC"

2. **Pide cambios incrementales**:
   > "Implementa solo la Fase 1 primero, quiero probar antes de continuar"

3. **Solicita código compilable**:
   > "Dame el código completo del archivo, no fragmentos"

4. **Verifica compatibilidad**:
   > "¿Este cambio rompe algo del código existente?"

5. **Para debugging de performance**:
   > "Agrega timestamps en cada etapa del pipeline para medir dónde está el cuello de botella"

---

## 🎯 Orden Recomendado de Comandos

```
1. "Lee y analiza todo el módulo encoder/"
2. "Implementa libyuv para conversión de color (Fase 2)"
3. "Elimina los Sleep() innecesarios (Fase 1)"
4. "Crea la clase NvencEncoder con la misma interfaz que SimpleVp8Encoder"
5. "Integra NVENC en el pipeline existente"
6. "Reemplaza StretchDIBits por Direct3D 11"
```
