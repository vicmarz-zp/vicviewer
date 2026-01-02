// Benchmark: Decoder optimizations - libyuv SIMD + buffer reuse
// Compara el rendimiento del decoder VP8 con las nuevas optimizaciones

#include "VideoDecoder.h"
#include "VideoEncoder.h"
#include "DesktopFrame.h"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>
#include <numeric>

using namespace std::chrono;

struct BenchmarkResult {
    std::string name;
    double avgMs;
    double minMs;
    double maxMs;
    int fps;
};

vic::capture::DesktopFrame createTestFrame(uint32_t width, uint32_t height) {
    vic::capture::DesktopFrame frame;
    frame.width = width;
    frame.height = height;
    frame.timestamp = 0;
    frame.bgraData.resize(static_cast<size_t>(width) * height * 4);
    
    // Patrón de gradiente para contenido realista
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            size_t idx = (static_cast<size_t>(y) * width + x) * 4;
            frame.bgraData[idx + 0] = static_cast<uint8_t>((x * 255) / width);      // B
            frame.bgraData[idx + 1] = static_cast<uint8_t>((y * 255) / height);     // G
            frame.bgraData[idx + 2] = static_cast<uint8_t>(((x + y) * 127) / (width + height)); // R
            frame.bgraData[idx + 3] = 255;  // A
        }
    }
    return frame;
}

BenchmarkResult benchmarkDecoder(uint32_t width, uint32_t height, int iterations) {
    std::cout << "\n=== Benchmark Decoder " << width << "x" << height << " ===" << std::endl;
    
    // Crear encoder y decoder
    auto encoder = vic::encoder::createVp8Encoder();
    auto decoder = vic::decoder::createVp8Decoder();
    
    if (!encoder || !decoder) {
        std::cerr << "Error: No se pudo crear encoder/decoder" << std::endl;
        return {"Error", 0, 0, 0, 0};
    }
    
    // Configurar encoder (usando API correcta)
    if (!encoder->Configure(width, height, 4000)) {
        std::cerr << "Error: No se pudo configurar encoder" << std::endl;
        return {"Error", 0, 0, 0, 0};
    }
    
    // Crear frame de prueba y encodear
    auto testFrame = createTestFrame(width, height);
    auto encoded = encoder->EncodeFrame(testFrame);
    
    if (!encoded) {
        std::cerr << "Error: No se pudo encodear frame de prueba" << std::endl;
        return {"Error", 0, 0, 0, 0};
    }
    
    std::cout << "Frame encoded: " << encoded->payload.size() << " bytes" << std::endl;
    
    // Warmup
    for (int i = 0; i < 10; ++i) {
        decoder->decode(*encoded);
    }
    
    // Benchmark decode
    std::vector<double> decodeTimes;
    decodeTimes.reserve(iterations);
    
    for (int i = 0; i < iterations; ++i) {
        auto start = high_resolution_clock::now();
        auto decoded = decoder->decode(*encoded);
        auto end = high_resolution_clock::now();
        
        if (!decoded) {
            std::cerr << "Error en decode iteracion " << i << std::endl;
            continue;
        }
        
        double ms = duration<double, std::milli>(end - start).count();
        decodeTimes.push_back(ms);
    }
    
    if (decodeTimes.empty()) {
        return {"Error", 0, 0, 0, 0};
    }
    
    // Calcular estadísticas
    double sum = std::accumulate(decodeTimes.begin(), decodeTimes.end(), 0.0);
    double avg = sum / decodeTimes.size();
    double minTime = *std::min_element(decodeTimes.begin(), decodeTimes.end());
    double maxTime = *std::max_element(decodeTimes.begin(), decodeTimes.end());
    int fps = static_cast<int>(1000.0 / avg);
    
    std::string name = std::to_string(width) + "x" + std::to_string(height);
    return {name, avg, minTime, maxTime, fps};
}

int main() {
    std::cout << "=======================================================" << std::endl;
    std::cout << "  VicViewer Decoder Benchmark (libyuv SIMD + reuse)   " << std::endl;
    std::cout << "=======================================================" << std::endl;
    
    const int ITERATIONS = 200;
    
    std::vector<BenchmarkResult> results;
    
    // Benchmark diferentes resoluciones
    results.push_back(benchmarkDecoder(640, 360, ITERATIONS));   // 360p
    results.push_back(benchmarkDecoder(854, 480, ITERATIONS));   // 480p
    results.push_back(benchmarkDecoder(1280, 720, ITERATIONS));  // 720p
    results.push_back(benchmarkDecoder(1920, 1080, ITERATIONS)); // 1080p
    
    // Tabla de resultados
    std::cout << "\n" << std::string(70, '=') << std::endl;
    std::cout << "                    RESULTADOS DECODER OPTIMIZADO" << std::endl;
    std::cout << std::string(70, '=') << std::endl;
    std::cout << std::left << std::setw(15) << "Resolution"
              << std::setw(12) << "Avg (ms)"
              << std::setw(12) << "Min (ms)"
              << std::setw(12) << "Max (ms)"
              << std::setw(10) << "FPS"
              << std::setw(10) << "Target" << std::endl;
    std::cout << std::string(70, '-') << std::endl;
    
    for (const auto& r : results) {
        std::string status = (r.fps >= 60) ? "OK" : (r.fps >= 30 ? "Med" : "Low");
        std::cout << std::left << std::setw(15) << r.name
                  << std::setw(12) << std::fixed << std::setprecision(2) << r.avgMs
                  << std::setw(12) << r.minMs
                  << std::setw(12) << r.maxMs
                  << std::setw(10) << r.fps
                  << std::setw(10) << status << std::endl;
    }
    
    std::cout << std::string(70, '=') << std::endl;
    std::cout << "\nOptimizaciones aplicadas:" << std::endl;
    std::cout << "  - libyuv::I420ToARGB (SIMD: SSE2/AVX2)" << std::endl;
    std::cout << "  - Buffer BGRA reutilizable (evita allocations)" << std::endl;
    std::cout << "  - Pre-allocation en configure()" << std::endl;
    
    return 0;
}
