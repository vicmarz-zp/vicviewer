#pragma once

#include "VideoEncoder.h"

#include <memory>
#include <optional>
#include <string>

// Forward declarations para evitar incluir headers de NVIDIA
struct ID3D11Device;
struct ID3D11Texture2D;

namespace vic::encoder {

/// Configuración para NVENC
struct NvencConfig {
    uint32_t targetBitrateKbps = 5000;
    uint32_t maxBitrateKbps = 8000;
    uint32_t gopLength = 60;           // Keyframe cada N frames
    bool useBFrames = false;           // Sin B-frames para menor latencia
    bool useHEVC = false;              // false = H.264, true = HEVC
    bool lowLatencyMode = true;        // Optimizado para streaming
    bool adaptiveQuantization = true;  // Mejor calidad visual
};

/// Verificar si NVENC está disponible en el sistema
bool isNvencAvailable();

/// Obtener información del encoder NVENC
std::string getNvencInfo();

/// Crear encoder NVENC.
/// Retorna nullptr si NVENC no está disponible o falla la inicialización.
/// @param device D3D11 device opcional - si es nullptr, crea uno interno
std::unique_ptr<VideoEncoder> createNvencEncoder(ID3D11Device* device = nullptr);

/// Crear encoder NVENC con configuración específica
std::unique_ptr<VideoEncoder> createNvencEncoder(const NvencConfig& config, ID3D11Device* device = nullptr);

/// Crear el mejor encoder disponible (NVENC si disponible, sino VP8)
std::unique_ptr<VideoEncoder> createBestEncoder();

} // namespace vic::encoder
