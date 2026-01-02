#pragma once

#include <cstdint>
#include <string>

namespace vic::pipeline {

/// Configuración de calidad del stream
enum class QualityPreset {
    Low,      // 540p, 1000 kbps - Para conexiones lentas
    Medium,   // 720p, 2000 kbps - Balance calidad/rendimiento  
    High,     // 1080p, 4000 kbps - LAN o conexiones rápidas
    Auto      // Adaptativo basado en RTT (futuro)
};

/// Configuración del stream de video/input
struct StreamConfig {
    // Video
    QualityPreset quality = QualityPreset::Medium;
    uint32_t maxWidth = 1280;      // Resolución máxima de salida
    uint32_t maxHeight = 720;
    uint32_t targetBitrateKbps = 2000;
    uint32_t maxFramerate = 30;    // FPS objetivo
    
    // Captura
    uint32_t captureTimeoutMs = 16;  // ~60 FPS máximo de captura
    bool enableCursorOverlay = true;
    
    // Input
    bool enableInputCoalescing = true;  // Agrupar eventos de mouse move
    uint32_t inputBatchIntervalMs = 5;  // Enviar batch cada 5ms
    
    // Métricas
    bool showMetrics = false;  // Mostrar FPS/latencia en pantalla
    
    /// Aplicar preset de calidad
    void applyPreset(QualityPreset preset) {
        quality = preset;
        switch (preset) {
        case QualityPreset::Low:
            maxWidth = 960;
            maxHeight = 540;
            targetBitrateKbps = 1000;
            maxFramerate = 24;
            break;
        case QualityPreset::Medium:
            maxWidth = 1280;
            maxHeight = 720;
            targetBitrateKbps = 2000;
            maxFramerate = 30;
            break;
        case QualityPreset::High:
            maxWidth = 1920;
            maxHeight = 1080;
            targetBitrateKbps = 4000;
            maxFramerate = 60;
            break;
        case QualityPreset::Auto:
            // Empezar en medium, ajustar dinámicamente
            maxWidth = 1280;
            maxHeight = 720;
            targetBitrateKbps = 2000;
            maxFramerate = 30;
            break;
        }
    }
    
    /// Crear config desde string (para línea de comandos)
    static StreamConfig fromPresetName(const std::string& name) {
        StreamConfig config;
        if (name == "low" || name == "bajo") {
            config.applyPreset(QualityPreset::Low);
        } else if (name == "high" || name == "alto") {
            config.applyPreset(QualityPreset::High);
        } else {
            config.applyPreset(QualityPreset::Medium);
        }
        return config;
    }
};

} // namespace vic::pipeline
