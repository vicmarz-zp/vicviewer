#include "FrameScaler.h"
#include "Logger.h"

#include <libyuv.h>
#include <algorithm>
#include <cstring>

namespace vic::capture {

struct FrameScaler::Impl {
    // Buffers reutilizables para evitar allocations
    std::vector<uint8_t> srcI420_;
    std::vector<uint8_t> dstI420_;
    std::vector<uint8_t> dstBgra_;
    
    uint32_t lastSrcWidth_ = 0;
    uint32_t lastSrcHeight_ = 0;
    uint32_t lastDstWidth_ = 0;
    uint32_t lastDstHeight_ = 0;
};

FrameScaler::FrameScaler() : impl_(std::make_unique<Impl>()) {}
FrameScaler::~FrameScaler() = default;

void FrameScaler::calculateScaledDimensions(
    uint32_t srcWidth, uint32_t srcHeight,
    uint32_t maxWidth, uint32_t maxHeight,
    uint32_t& outWidth, uint32_t& outHeight) {
    
    // Si ya cabe, no escalar
    if (srcWidth <= maxWidth && srcHeight <= maxHeight) {
        outWidth = srcWidth;
        outHeight = srcHeight;
        return;
    }
    
    // Calcular factor de escala manteniendo aspect ratio
    float scaleX = static_cast<float>(maxWidth) / srcWidth;
    float scaleY = static_cast<float>(maxHeight) / srcHeight;
    float scale = std::min(scaleX, scaleY);
    
    outWidth = static_cast<uint32_t>(srcWidth * scale);
    outHeight = static_cast<uint32_t>(srcHeight * scale);
    
    // Asegurar que sean múltiplos de 2 (requerido por codecs de video)
    outWidth = (outWidth + 1) & ~1u;
    outHeight = (outHeight + 1) & ~1u;
    
    // Mínimo 320x180
    outWidth = std::max(outWidth, 320u);
    outHeight = std::max(outHeight, 180u);
}

std::unique_ptr<DesktopFrame> FrameScaler::scale(
    const DesktopFrame& source,
    uint32_t targetWidth,
    uint32_t targetHeight) {
    
    // Calcular dimensiones reales manteniendo aspect ratio
    uint32_t scaledWidth, scaledHeight;
    calculateScaledDimensions(source.width, source.height, 
                              targetWidth, targetHeight,
                              scaledWidth, scaledHeight);
    
    // Si no hay que escalar, hacer copia directa
    if (scaledWidth == source.width && scaledHeight == source.height) {
        auto result = std::make_unique<DesktopFrame>();
        result->width = source.width;
        result->height = source.height;
        result->timestamp = source.timestamp;
        result->bgraData = source.bgraData;
        return result;
    }
    
    // Preparar buffers si cambiaron dimensiones
    const size_t srcYSize = static_cast<size_t>(source.width) * source.height;
    const size_t srcUvSize = ((source.width + 1) / 2) * ((source.height + 1) / 2);
    const size_t srcI420Size = srcYSize + srcUvSize * 2;
    
    const size_t dstYSize = static_cast<size_t>(scaledWidth) * scaledHeight;
    const size_t dstUvSize = ((scaledWidth + 1) / 2) * ((scaledHeight + 1) / 2);
    const size_t dstI420Size = dstYSize + dstUvSize * 2;
    const size_t dstBgraSize = static_cast<size_t>(scaledWidth) * scaledHeight * 4;
    
    if (impl_->srcI420_.size() < srcI420Size) {
        impl_->srcI420_.resize(srcI420Size);
    }
    if (impl_->dstI420_.size() < dstI420Size) {
        impl_->dstI420_.resize(dstI420Size);
    }
    if (impl_->dstBgra_.size() < dstBgraSize) {
        impl_->dstBgra_.resize(dstBgraSize);
    }
    
    // Paso 1: BGRA -> I420 (source)
    uint8_t* srcY = impl_->srcI420_.data();
    uint8_t* srcU = srcY + srcYSize;
    uint8_t* srcV = srcU + srcUvSize;
    
    int srcStrideY = static_cast<int>(source.width);
    int srcStrideU = static_cast<int>((source.width + 1) / 2);
    int srcStrideV = srcStrideU;
    
    // libyuv: ARGB significa BGRA en memoria (little-endian Windows)
    libyuv::ARGBToI420(
        source.bgraData.data(), static_cast<int>(source.width * 4),
        srcY, srcStrideY,
        srcU, srcStrideU,
        srcV, srcStrideV,
        static_cast<int>(source.width),
        static_cast<int>(source.height));
    
    // Paso 2: Escalar I420
    uint8_t* dstY = impl_->dstI420_.data();
    uint8_t* dstU = dstY + dstYSize;
    uint8_t* dstV = dstU + dstUvSize;
    
    int dstStrideY = static_cast<int>(scaledWidth);
    int dstStrideU = static_cast<int>((scaledWidth + 1) / 2);
    int dstStrideV = dstStrideU;
    
    // Usar filtrado bilineal para mejor calidad
    libyuv::I420Scale(
        srcY, srcStrideY,
        srcU, srcStrideU,
        srcV, srcStrideV,
        static_cast<int>(source.width),
        static_cast<int>(source.height),
        dstY, dstStrideY,
        dstU, dstStrideU,
        dstV, dstStrideV,
        static_cast<int>(scaledWidth),
        static_cast<int>(scaledHeight),
        libyuv::kFilterBilinear);
    
    // Paso 3: I420 -> BGRA (destino)
    libyuv::I420ToARGB(
        dstY, dstStrideY,
        dstU, dstStrideU,
        dstV, dstStrideV,
        impl_->dstBgra_.data(), static_cast<int>(scaledWidth * 4),
        static_cast<int>(scaledWidth),
        static_cast<int>(scaledHeight));
    
    // Crear frame de salida
    auto result = std::make_unique<DesktopFrame>();
    result->width = scaledWidth;
    result->height = scaledHeight;
    result->timestamp = source.timestamp;
    result->bgraData.assign(impl_->dstBgra_.begin(), 
                            impl_->dstBgra_.begin() + dstBgraSize);
    
    // Log primera vez que se escala
    if (impl_->lastDstWidth_ != scaledWidth || impl_->lastDstHeight_ != scaledHeight) {
        logging::global().log(logging::Logger::Level::Info,
            "FrameScaler: " + std::to_string(source.width) + "x" + std::to_string(source.height) +
            " -> " + std::to_string(scaledWidth) + "x" + std::to_string(scaledHeight));
        impl_->lastDstWidth_ = scaledWidth;
        impl_->lastDstHeight_ = scaledHeight;
    }
    
    return result;
}

} // namespace vic::capture
