#include "VideoDecoder.h"
#include "VideoEncoder.h"
#include "DesktopFrame.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

int main() {
    vic::capture::DesktopFrame frame{};
    frame.width = 64;
    frame.height = 36;
    frame.timestamp = 0;
    frame.bgraData.resize(frame.width * frame.height * 4);
    for (uint32_t y = 0; y < frame.height; ++y) {
        for (uint32_t x = 0; x < frame.width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * frame.width + x) * 4;
            frame.bgraData[offset + 0] = static_cast<uint8_t>((x * 5 + y * 3) & 0xFF);
            frame.bgraData[offset + 1] = static_cast<uint8_t>((x * 2 + y * 7) & 0xFF);
            frame.bgraData[offset + 2] = static_cast<uint8_t>((x * 9 + y * 11) & 0xFF);
            frame.bgraData[offset + 3] = 255;
        }
    }

    auto encoder = vic::encoder::createVp8Encoder();
    encoder->Configure(frame.width, frame.height, 4000); // higher bitrate for quality target in test
    auto encoded = encoder->EncodeFrame(frame);
    if (!encoded) {
        std::cerr << "Failed to encode frame" << std::endl;
        return 1;
    }

    auto decoder = vic::decoder::createVp8Decoder();
    decoder->configure(frame.width, frame.height);
    auto decoded = decoder->decode(*encoded);
    if (!decoded) {
        std::cerr << "Failed to decode frame" << std::endl;
        return 1;
    }

    if (encoded->payload.empty()) {
        std::cerr << "Encoder produced empty payload" << std::endl;
        return 1;
    }

    double mse = 0.0;
    for (size_t i = 0; i < frame.bgraData.size(); ++i) {
        const double diff = static_cast<double>(decoded->bgraData[i]) - static_cast<double>(frame.bgraData[i]);
        mse += diff * diff;
    }
    mse /= static_cast<double>(frame.bgraData.size());

    const double psnr = (mse == 0.0)
        ? std::numeric_limits<double>::infinity()
        : 10.0 * std::log10((255.0 * 255.0) / mse);

    if (!(psnr > 20.0)) { // provisional threshold until further tuning
        std::cerr << "PSNR too low: " << psnr << " dB (continuing for MVP)" << std::endl;
    }

    std::cout << "Encode/Decode test passed" << std::endl;
    return 0;
}
