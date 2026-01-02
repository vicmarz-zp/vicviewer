#pragma once

#include "DesktopFrame.h"
#include <memory>

namespace vic::capture {

/// Escalador de frames usando libyuv (SIMD optimizado)
class FrameScaler {
public:
    FrameScaler();
    ~FrameScaler();
    
    /// Escalar frame a resolución objetivo
    /// Si el frame ya es menor o igual, retorna una copia sin escalar
    std::unique_ptr<DesktopFrame> scale(
        const DesktopFrame& source,
        uint32_t targetWidth,
        uint32_t targetHeight);
    
    /// Calcular dimensiones manteniendo aspect ratio
    static void calculateScaledDimensions(
        uint32_t srcWidth, uint32_t srcHeight,
        uint32_t maxWidth, uint32_t maxHeight,
        uint32_t& outWidth, uint32_t& outHeight);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vic::capture
