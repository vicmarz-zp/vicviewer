#pragma once

#include "EncodedFrame.h"
#include "DesktopFrame.h"

#include <optional>
#include <memory>

namespace vic::decoder {

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;

    virtual bool configure(uint32_t width, uint32_t height) = 0;
    virtual std::optional<vic::capture::DesktopFrame> decode(const vic::encoder::EncodedFrame& frame) = 0;
};

std::unique_ptr<VideoDecoder> createVp8Decoder();

} // namespace vic::decoder
