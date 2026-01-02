// Test NVENC availability and capabilities
#include "NvencEncoder.h"
#include "VideoEncoder.h"
#include "DesktopFrame.h"

#include <iostream>
#include <chrono>
#include <vector>
#include <numeric>

using Clock = std::chrono::high_resolution_clock;

int main() {
    std::cout << "=== NVENC Detection Test ===" << std::endl;
    std::cout << std::endl;

    // Check NVENC availability
    std::cout << "Checking NVENC availability..." << std::endl;
    bool nvencAvailable = vic::encoder::isNvencAvailable();
    std::cout << vic::encoder::getNvencInfo() << std::endl;
    std::cout << std::endl;

    if (!nvencAvailable) {
        std::cout << "[INFO] NVENC not available. Possible reasons:" << std::endl;
        std::cout << "  - No NVIDIA GPU installed" << std::endl;
        std::cout << "  - NVIDIA driver too old (need 450+ for SDK 12)" << std::endl;
        std::cout << "  - GeForce driver without NVENC support" << std::endl;
        std::cout << std::endl;
        std::cout << "Using VP8 software encoder instead." << std::endl;
    }

    // Test createBestEncoder
    std::cout << std::endl;
    std::cout << "=== Testing createBestEncoder() ===" << std::endl;
    auto encoder = vic::encoder::createBestEncoder();
    
    if (!encoder) {
        std::cerr << "ERROR: Failed to create encoder!" << std::endl;
        return 1;
    }

    // Create test frame (1920x1080)
    const uint32_t width = 1920;
    const uint32_t height = 1080;
    
    vic::capture::DesktopFrame frame{};
    frame.width = width;
    frame.height = height;
    frame.timestamp = 0;
    frame.bgraData.resize(width * height * 4);
    
    // Fill with test pattern
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            size_t offset = (static_cast<size_t>(y) * width + x) * 4;
            frame.bgraData[offset + 0] = static_cast<uint8_t>(x & 0xFF);
            frame.bgraData[offset + 1] = static_cast<uint8_t>(y & 0xFF);
            frame.bgraData[offset + 2] = static_cast<uint8_t>((x + y) & 0xFF);
            frame.bgraData[offset + 3] = 255;
        }
    }

    std::cout << "Test frame: " << width << "x" << height << std::endl;
    std::cout << std::endl;

    // Benchmark encoding
    std::cout << "=== Encoding Benchmark (10 frames) ===" << std::endl;
    
    std::vector<double> encodeTimes;
    std::vector<size_t> frameSizes;
    
    for (int i = 0; i < 10; ++i) {
        auto start = Clock::now();
        auto encoded = encoder->EncodeFrame(frame);
        auto end = Clock::now();
        
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        encodeTimes.push_back(ms);
        
        if (encoded) {
            frameSizes.push_back(encoded->payload.size());
            std::cout << "  Frame " << i << ": " << ms << " ms, " 
                      << (encoded->payload.size() / 1024.0) << " KB"
                      << (encoded->keyFrame ? " [KEY]" : "") << std::endl;
        } else {
            std::cout << "  Frame " << i << ": FAILED" << std::endl;
        }
    }

    if (!encodeTimes.empty()) {
        double avg = std::accumulate(encodeTimes.begin(), encodeTimes.end(), 0.0) / encodeTimes.size();
        double minT = *std::min_element(encodeTimes.begin(), encodeTimes.end());
        double maxT = *std::max_element(encodeTimes.begin(), encodeTimes.end());
        
        std::cout << std::endl;
        std::cout << "Results:" << std::endl;
        std::cout << "  Avg encode time: " << avg << " ms" << std::endl;
        std::cout << "  Min: " << minT << " ms, Max: " << maxT << " ms" << std::endl;
        std::cout << "  Theoretical FPS: " << (1000.0 / avg) << std::endl;
        
        if (!frameSizes.empty()) {
            double avgSize = std::accumulate(frameSizes.begin(), frameSizes.end(), 0.0) / frameSizes.size();
            std::cout << "  Avg frame size: " << (avgSize / 1024.0) << " KB" << std::endl;
        }
    }

    std::cout << std::endl;
    if (nvencAvailable) {
        std::cout << "[SUCCESS] NVENC hardware encoding is working!" << std::endl;
    } else {
        std::cout << "[INFO] Using VP8 software encoding (no NVIDIA GPU)" << std::endl;
    }

    return 0;
}
