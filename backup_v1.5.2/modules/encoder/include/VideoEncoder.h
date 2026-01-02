#pragma once

#include "EncodedFrame.h"

#include "DesktopFrame.h"

#include <optional>
#include <memory>
#include <vector>

namespace vic::encoder {

class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;

    virtual bool Configure(uint32_t width, uint32_t height, uint32_t targetBitrateKbps) = 0;
    virtual std::optional<EncodedFrame> EncodeFrame(const vic::capture::DesktopFrame& frame) = 0;
    virtual std::vector<uint8_t> Flush() = 0;
    
    /// Forzar que el próximo frame sea un keyframe
    virtual void forceNextKeyframe() { forceKeyframe_ = true; }

protected:
    bool forceKeyframe_ = false;
};

/// Crear encoder VP8 (software, siempre disponible)
std::unique_ptr<VideoEncoder> createVp8Encoder();

/// Crear el mejor encoder disponible (NVENC hardware si disponible, sino VP8)
std::unique_ptr<VideoEncoder> createBestEncoder();

} // namespace vic::encoder
