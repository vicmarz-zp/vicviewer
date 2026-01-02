#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <deque>

namespace vic::metrics {

/// Métricas de rendimiento del pipeline de video
struct PipelineMetrics {
    // Tiempos promedio en microsegundos
    double avgCaptureTimeUs = 0;
    double avgColorConvertTimeUs = 0;
    double avgEncodeTimeUs = 0;
    double avgNetworkTimeUs = 0;
    double avgDecodeTimeUs = 0;
    double avgRenderTimeUs = 0;
    
    // Latencia total (glass-to-glass estimada)
    double avgTotalLatencyMs = 0;
    
    // Frame rate
    double currentFps = 0;
    double avgFps = 0;
    
    // Tamaño de frames
    double avgFrameSizeBytes = 0;
    double avgBitrateKbps = 0;
    
    // Contadores
    uint64_t totalFramesCaptured = 0;
    uint64_t totalFramesEncoded = 0;
    uint64_t totalFramesDropped = 0;
    uint64_t totalBytesTransferred = 0;
};

/// Singleton para recopilar métricas del pipeline
class MetricsCollector {
public:
    static MetricsCollector& instance();

    // Marcar inicio de cada fase
    void markCaptureStart();
    void markCaptureEnd();
    void markColorConvertStart();
    void markColorConvertEnd();
    void markEncodeStart();
    void markEncodeEnd();
    void markNetworkSendStart();
    void markNetworkReceiveEnd();
    void markDecodeStart();
    void markDecodeEnd();
    void markRenderStart();
    void markRenderEnd();
    
    // Registrar tamaño de frame
    void recordFrameSize(size_t bytes);
    void recordFrameDropped();
    
    // Obtener métricas actuales
    PipelineMetrics getMetrics() const;
    
    // Formatear métricas como string para display
    std::string formatMetrics() const;
    
    // Reset métricas
    void reset();

private:
    MetricsCollector() = default;
    
    void updateAverages();
    
    mutable std::mutex mutex_;
    
    // Timestamps de la fase actual
    std::chrono::steady_clock::time_point captureStart_;
    std::chrono::steady_clock::time_point colorConvertStart_;
    std::chrono::steady_clock::time_point encodeStart_;
    std::chrono::steady_clock::time_point networkSendStart_;
    std::chrono::steady_clock::time_point decodeStart_;
    std::chrono::steady_clock::time_point renderStart_;
    
    // Últimas N mediciones para promedios
    static constexpr size_t kWindowSize = 60;  // ~1 segundo a 60fps
    std::deque<double> captureTimesUs_;
    std::deque<double> colorConvertTimesUs_;
    std::deque<double> encodeTimesUs_;
    std::deque<double> networkTimesUs_;
    std::deque<double> decodeTimesUs_;
    std::deque<double> renderTimesUs_;
    std::deque<size_t> frameSizes_;
    std::deque<std::chrono::steady_clock::time_point> frameTimestamps_;
    
    PipelineMetrics currentMetrics_{};
};

/// Helper RAII para medir tiempo de una sección
class ScopedTimer {
public:
    using Callback = void(MetricsCollector::*)();
    
    ScopedTimer(Callback startFn, Callback endFn)
        : endFn_(endFn) {
        (MetricsCollector::instance().*startFn)();
    }
    
    ~ScopedTimer() {
        (MetricsCollector::instance().*endFn_)();
    }
    
private:
    Callback endFn_;
};

// Macros de conveniencia
#define VIC_METRICS_CAPTURE() \
    vic::metrics::ScopedTimer _capture_timer_( \
        &vic::metrics::MetricsCollector::markCaptureStart, \
        &vic::metrics::MetricsCollector::markCaptureEnd)

#define VIC_METRICS_COLOR_CONVERT() \
    vic::metrics::ScopedTimer _color_timer_( \
        &vic::metrics::MetricsCollector::markColorConvertStart, \
        &vic::metrics::MetricsCollector::markColorConvertEnd)

#define VIC_METRICS_ENCODE() \
    vic::metrics::ScopedTimer _encode_timer_( \
        &vic::metrics::MetricsCollector::markEncodeStart, \
        &vic::metrics::MetricsCollector::markEncodeEnd)

#define VIC_METRICS_DECODE() \
    vic::metrics::ScopedTimer _decode_timer_( \
        &vic::metrics::MetricsCollector::markDecodeStart, \
        &vic::metrics::MetricsCollector::markDecodeEnd)

#define VIC_METRICS_RENDER() \
    vic::metrics::ScopedTimer _render_timer_( \
        &vic::metrics::MetricsCollector::markRenderStart, \
        &vic::metrics::MetricsCollector::markRenderEnd)

} // namespace vic::metrics
