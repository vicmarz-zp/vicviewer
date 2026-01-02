// Quick performance benchmark for VicViewer optimizations
#include "VideoEncoder.h"
#include "VideoDecoder.h"
#include "ColorConvert.h"
#include "DesktopFrame.h"

#include <chrono>
#include <iostream>
#include <vector>
#include <numeric>

using Clock = std::chrono::high_resolution_clock;

int main() {
    std::cout << "=== VicViewer Performance Benchmark ===" << std::endl;
    
    // Test at 1920x1080 (Full HD)
    const uint32_t width = 1920;
    const uint32_t height = 1080;
    const int iterations = 50;
    
    // Create test frame
    vic::capture::DesktopFrame frame{};
    frame.width = width;
    frame.height = height;
    frame.timestamp = 0;
    frame.bgraData.resize(width * height * 4);
    
    // Fill with gradient pattern
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
            frame.bgraData[offset + 0] = static_cast<uint8_t>(x & 0xFF);  // B
            frame.bgraData[offset + 1] = static_cast<uint8_t>(y & 0xFF);  // G
            frame.bgraData[offset + 2] = static_cast<uint8_t>((x + y) & 0xFF);  // R
            frame.bgraData[offset + 3] = 255;  // A
        }
    }
    
    std::cout << "Resolution: " << width << "x" << height << std::endl;
    std::cout << "Iterations: " << iterations << std::endl;
    std::cout << std::endl;
    
    // === Benchmark Color Conversion ===
    std::cout << "--- Color Conversion (BGRA -> I420) ---" << std::endl;
    auto colorConverter = vic::encoder::createColorConverter();
    
    std::vector<uint8_t> yPlane(width * height);
    std::vector<uint8_t> uPlane(width * height / 4);
    std::vector<uint8_t> vPlane(width * height / 4);
    
    std::vector<double> colorTimes;
    for (int i = 0; i < iterations; ++i) {
        auto start = Clock::now();
        colorConverter->BGRAToI420(
            frame.bgraData.data(), width * 4,
            yPlane.data(), width,
            uPlane.data(), width / 2,
            vPlane.data(), width / 2,
            width, height
        );
        auto end = Clock::now();
        colorTimes.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    
    double avgColor = std::accumulate(colorTimes.begin(), colorTimes.end(), 0.0) / colorTimes.size();
    double minColor = *std::min_element(colorTimes.begin(), colorTimes.end());
    double maxColor = *std::max_element(colorTimes.begin(), colorTimes.end());
    std::cout << "  Avg: " << avgColor << " ms" << std::endl;
    std::cout << "  Min: " << minColor << " ms" << std::endl;
    std::cout << "  Max: " << maxColor << " ms" << std::endl;
    std::cout << std::endl;
    
    // === Benchmark VP8 Encoding ===
    std::cout << "--- VP8 Encoding ---" << std::endl;
    auto encoder = vic::encoder::createVp8Encoder();
    encoder->Configure(width, height, 5000);
    
    // Warm up (first frame is keyframe, slower)
    encoder->EncodeFrame(frame);
    
    std::vector<double> encodeTimes;
    std::vector<size_t> frameSizes;
    for (int i = 0; i < iterations; ++i) {
        auto start = Clock::now();
        auto encoded = encoder->EncodeFrame(frame);
        auto end = Clock::now();
        encodeTimes.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        if (encoded) {
            frameSizes.push_back(encoded->payload.size());
        }
    }
    
    double avgEncode = std::accumulate(encodeTimes.begin(), encodeTimes.end(), 0.0) / encodeTimes.size();
    double minEncode = *std::min_element(encodeTimes.begin(), encodeTimes.end());
    double maxEncode = *std::max_element(encodeTimes.begin(), encodeTimes.end());
    double avgSize = std::accumulate(frameSizes.begin(), frameSizes.end(), 0.0) / frameSizes.size();
    
    std::cout << "  Avg: " << avgEncode << " ms" << std::endl;
    std::cout << "  Min: " << minEncode << " ms" << std::endl;
    std::cout << "  Max: " << maxEncode << " ms" << std::endl;
    std::cout << "  Avg frame size: " << (avgSize / 1024.0) << " KB" << std::endl;
    std::cout << std::endl;
    
    // === Benchmark VP8 Decoding ===
    std::cout << "--- VP8 Decoding ---" << std::endl;
    auto decoder = vic::decoder::createVp8Decoder();
    decoder->configure(width, height);
    
    // Get a sample encoded frame
    auto sampleEncoded = encoder->EncodeFrame(frame);
    
    std::vector<double> decodeTimes;
    for (int i = 0; i < iterations; ++i) {
        auto start = Clock::now();
        auto decoded = decoder->decode(*sampleEncoded);
        auto end = Clock::now();
        decodeTimes.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    
    double avgDecode = std::accumulate(decodeTimes.begin(), decodeTimes.end(), 0.0) / decodeTimes.size();
    double minDecode = *std::min_element(decodeTimes.begin(), decodeTimes.end());
    double maxDecode = *std::max_element(decodeTimes.begin(), decodeTimes.end());
    
    std::cout << "  Avg: " << avgDecode << " ms" << std::endl;
    std::cout << "  Min: " << minDecode << " ms" << std::endl;
    std::cout << "  Max: " << maxDecode << " ms" << std::endl;
    std::cout << std::endl;
    
    // === Summary ===
    double totalPipeline = avgColor + avgEncode + avgDecode;
    double maxFps = 1000.0 / totalPipeline;
    
    std::cout << "=== SUMMARY ===" << std::endl;
    std::cout << "Total pipeline (color + encode + decode): " << totalPipeline << " ms" << std::endl;
    std::cout << "Theoretical max FPS: " << maxFps << std::endl;
    std::cout << std::endl;
    
    if (maxFps >= 60.0) {
        std::cout << "[OK] Pipeline can sustain 60 FPS!" << std::endl;
    } else if (maxFps >= 30.0) {
        std::cout << "[WARN] Pipeline can sustain 30 FPS but not 60 FPS" << std::endl;
    } else {
        std::cout << "[SLOW] Pipeline below 30 FPS target" << std::endl;
    }
    
    return 0;
}
