// Multi-resolution benchmark
#include "VideoEncoder.h"
#include "VideoDecoder.h"
#include "ColorConvert.h"
#include "DesktopFrame.h"

#include <chrono>
#include <iostream>
#include <vector>
#include <numeric>

using Clock = std::chrono::high_resolution_clock;

struct Resolution {
    uint32_t width;
    uint32_t height;
    const char* name;
};

void benchmarkResolution(const Resolution& res, int iterations = 20) {
    std::cout << "--- " << res.name << " (" << res.width << "x" << res.height << ") ---" << std::endl;
    
    // Create test frame
    vic::capture::DesktopFrame frame{};
    frame.width = res.width;
    frame.height = res.height;
    frame.timestamp = 0;
    frame.bgraData.resize(res.width * res.height * 4);
    
    for (uint32_t y = 0; y < res.height; ++y) {
        for (uint32_t x = 0; x < res.width; ++x) {
            size_t offset = (static_cast<size_t>(y) * res.width + x) * 4;
            frame.bgraData[offset + 0] = static_cast<uint8_t>(x & 0xFF);
            frame.bgraData[offset + 1] = static_cast<uint8_t>(y & 0xFF);
            frame.bgraData[offset + 2] = static_cast<uint8_t>((x + y) & 0xFF);
            frame.bgraData[offset + 3] = 255;
        }
    }

    // Color conversion
    auto colorConverter = vic::encoder::createColorConverter();
    std::vector<uint8_t> yPlane(res.width * res.height);
    std::vector<uint8_t> uPlane(res.width * res.height / 4);
    std::vector<uint8_t> vPlane(res.width * res.height / 4);
    
    std::vector<double> colorTimes;
    for (int i = 0; i < iterations; ++i) {
        auto start = Clock::now();
        colorConverter->BGRAToI420(
            frame.bgraData.data(), res.width * 4,
            yPlane.data(), res.width,
            uPlane.data(), res.width / 2,
            vPlane.data(), res.width / 2,
            res.width, res.height
        );
        auto end = Clock::now();
        colorTimes.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }

    // VP8 encoding
    auto encoder = vic::encoder::createVp8Encoder();
    encoder->Configure(res.width, res.height, 5000);
    encoder->EncodeFrame(frame); // Warm up
    
    std::vector<double> encodeTimes;
    std::vector<size_t> frameSizes;
    for (int i = 0; i < iterations; ++i) {
        auto start = Clock::now();
        auto encoded = encoder->EncodeFrame(frame);
        auto end = Clock::now();
        encodeTimes.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        if (encoded) frameSizes.push_back(encoded->payload.size());
    }

    // VP8 decoding
    auto decoder = vic::decoder::createVp8Decoder();
    decoder->configure(res.width, res.height);
    auto sampleEncoded = encoder->EncodeFrame(frame);
    
    std::vector<double> decodeTimes;
    for (int i = 0; i < iterations; ++i) {
        auto start = Clock::now();
        decoder->decode(*sampleEncoded);
        auto end = Clock::now();
        decodeTimes.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }

    // Calculate averages
    double avgColor = std::accumulate(colorTimes.begin(), colorTimes.end(), 0.0) / colorTimes.size();
    double avgEncode = std::accumulate(encodeTimes.begin(), encodeTimes.end(), 0.0) / encodeTimes.size();
    double avgDecode = std::accumulate(decodeTimes.begin(), decodeTimes.end(), 0.0) / decodeTimes.size();
    double avgSize = frameSizes.empty() ? 0 : std::accumulate(frameSizes.begin(), frameSizes.end(), 0.0) / frameSizes.size();

    double totalPipeline = avgColor + avgEncode + avgDecode;
    double maxFps = 1000.0 / totalPipeline;

    std::cout << "  Color:  " << avgColor << " ms" << std::endl;
    std::cout << "  Encode: " << avgEncode << " ms" << std::endl;
    std::cout << "  Decode: " << avgDecode << " ms" << std::endl;
    std::cout << "  Total:  " << totalPipeline << " ms" << std::endl;
    std::cout << "  FPS:    " << maxFps << std::endl;
    std::cout << "  Size:   " << (avgSize / 1024.0) << " KB/frame" << std::endl;
    
    if (maxFps >= 60) {
        std::cout << "  [OK] 60 FPS achievable!" << std::endl;
    } else if (maxFps >= 30) {
        std::cout << "  [OK] 30 FPS achievable" << std::endl;
    } else {
        std::cout << "  [WARN] Below 30 FPS" << std::endl;
    }
    std::cout << std::endl;
}

int main() {
    std::cout << "=== Multi-Resolution Benchmark ===" << std::endl;
    std::cout << std::endl;

    Resolution resolutions[] = {
        {640, 360, "360p (nHD)"},
        {854, 480, "480p (FWVGA)"},
        {1280, 720, "720p (HD)"},
        {1920, 1080, "1080p (Full HD)"},
    };

    for (const auto& res : resolutions) {
        benchmarkResolution(res);
    }

    std::cout << "=== Recommendation ===" << std::endl;
    std::cout << "For 60 FPS with VP8 software encoding:" << std::endl;
    std::cout << "  - Use 720p or lower resolution" << std::endl;
    std::cout << "  - Or use NVENC hardware encoding for 1080p" << std::endl;

    return 0;
}
