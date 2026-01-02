#include "Metrics.h"

#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace vic::metrics {

MetricsCollector& MetricsCollector::instance() {
    static MetricsCollector instance;
    return instance;
}

void MetricsCollector::markCaptureStart() {
    std::lock_guard<std::mutex> lock(mutex_);
    captureStart_ = std::chrono::steady_clock::now();
}

void MetricsCollector::markCaptureEnd() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto duration = std::chrono::steady_clock::now() - captureStart_;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    
    captureTimesUs_.push_back(static_cast<double>(us));
    if (captureTimesUs_.size() > kWindowSize) {
        captureTimesUs_.pop_front();
    }
    
    currentMetrics_.totalFramesCaptured++;
    frameTimestamps_.push_back(std::chrono::steady_clock::now());
    if (frameTimestamps_.size() > kWindowSize) {
        frameTimestamps_.pop_front();
    }
    
    updateAverages();
}

void MetricsCollector::markColorConvertStart() {
    std::lock_guard<std::mutex> lock(mutex_);
    colorConvertStart_ = std::chrono::steady_clock::now();
}

void MetricsCollector::markColorConvertEnd() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto duration = std::chrono::steady_clock::now() - colorConvertStart_;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    
    colorConvertTimesUs_.push_back(static_cast<double>(us));
    if (colorConvertTimesUs_.size() > kWindowSize) {
        colorConvertTimesUs_.pop_front();
    }
}

void MetricsCollector::markEncodeStart() {
    std::lock_guard<std::mutex> lock(mutex_);
    encodeStart_ = std::chrono::steady_clock::now();
}

void MetricsCollector::markEncodeEnd() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto duration = std::chrono::steady_clock::now() - encodeStart_;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    
    encodeTimesUs_.push_back(static_cast<double>(us));
    if (encodeTimesUs_.size() > kWindowSize) {
        encodeTimesUs_.pop_front();
    }
    
    currentMetrics_.totalFramesEncoded++;
}

void MetricsCollector::markNetworkSendStart() {
    std::lock_guard<std::mutex> lock(mutex_);
    networkSendStart_ = std::chrono::steady_clock::now();
}

void MetricsCollector::markNetworkReceiveEnd() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto duration = std::chrono::steady_clock::now() - networkSendStart_;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    
    networkTimesUs_.push_back(static_cast<double>(us));
    if (networkTimesUs_.size() > kWindowSize) {
        networkTimesUs_.pop_front();
    }
}

void MetricsCollector::markDecodeStart() {
    std::lock_guard<std::mutex> lock(mutex_);
    decodeStart_ = std::chrono::steady_clock::now();
}

void MetricsCollector::markDecodeEnd() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto duration = std::chrono::steady_clock::now() - decodeStart_;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    
    decodeTimesUs_.push_back(static_cast<double>(us));
    if (decodeTimesUs_.size() > kWindowSize) {
        decodeTimesUs_.pop_front();
    }
}

void MetricsCollector::markRenderStart() {
    std::lock_guard<std::mutex> lock(mutex_);
    renderStart_ = std::chrono::steady_clock::now();
}

void MetricsCollector::markRenderEnd() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto duration = std::chrono::steady_clock::now() - renderStart_;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    
    renderTimesUs_.push_back(static_cast<double>(us));
    if (renderTimesUs_.size() > kWindowSize) {
        renderTimesUs_.pop_front();
    }
}

void MetricsCollector::recordFrameSize(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    frameSizes_.push_back(bytes);
    if (frameSizes_.size() > kWindowSize) {
        frameSizes_.pop_front();
    }
    
    currentMetrics_.totalBytesTransferred += bytes;
    updateAverages();
}

void MetricsCollector::recordFrameDropped() {
    std::lock_guard<std::mutex> lock(mutex_);
    currentMetrics_.totalFramesDropped++;
}

void MetricsCollector::updateAverages() {
    // Calculate averages
    auto calcAvg = [](const std::deque<double>& data) -> double {
        if (data.empty()) return 0;
        return std::accumulate(data.begin(), data.end(), 0.0) / data.size();
    };
    
    currentMetrics_.avgCaptureTimeUs = calcAvg(captureTimesUs_);
    currentMetrics_.avgColorConvertTimeUs = calcAvg(colorConvertTimesUs_);
    currentMetrics_.avgEncodeTimeUs = calcAvg(encodeTimesUs_);
    currentMetrics_.avgNetworkTimeUs = calcAvg(networkTimesUs_);
    currentMetrics_.avgDecodeTimeUs = calcAvg(decodeTimesUs_);
    currentMetrics_.avgRenderTimeUs = calcAvg(renderTimesUs_);
    
    // Total latency
    currentMetrics_.avgTotalLatencyMs = (
        currentMetrics_.avgCaptureTimeUs +
        currentMetrics_.avgColorConvertTimeUs +
        currentMetrics_.avgEncodeTimeUs +
        currentMetrics_.avgNetworkTimeUs +
        currentMetrics_.avgDecodeTimeUs +
        currentMetrics_.avgRenderTimeUs
    ) / 1000.0;
    
    // Frame size average
    if (!frameSizes_.empty()) {
        double sum = 0;
        for (auto s : frameSizes_) sum += static_cast<double>(s);
        currentMetrics_.avgFrameSizeBytes = sum / frameSizes_.size();
    }
    
    // FPS calculation
    if (frameTimestamps_.size() >= 2) {
        auto duration = frameTimestamps_.back() - frameTimestamps_.front();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
        if (ms > 0) {
            currentMetrics_.avgFps = (frameTimestamps_.size() - 1) * 1000.0 / ms;
        }
    }
    
    // Bitrate
    if (currentMetrics_.avgFps > 0 && currentMetrics_.avgFrameSizeBytes > 0) {
        currentMetrics_.avgBitrateKbps = 
            (currentMetrics_.avgFrameSizeBytes * 8 * currentMetrics_.avgFps) / 1000.0;
    }
}

PipelineMetrics MetricsCollector::getMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentMetrics_;
}

std::string MetricsCollector::formatMetrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    
    ss << "=== VicViewer Performance Metrics ===\n";
    ss << "FPS: " << currentMetrics_.avgFps << "\n";
    ss << "Total Latency: " << currentMetrics_.avgTotalLatencyMs << " ms\n";
    ss << "\n--- Pipeline Breakdown ---\n";
    ss << "  Capture:      " << (currentMetrics_.avgCaptureTimeUs / 1000.0) << " ms\n";
    ss << "  Color Conv:   " << (currentMetrics_.avgColorConvertTimeUs / 1000.0) << " ms\n";
    ss << "  Encode:       " << (currentMetrics_.avgEncodeTimeUs / 1000.0) << " ms\n";
    ss << "  Network:      " << (currentMetrics_.avgNetworkTimeUs / 1000.0) << " ms\n";
    ss << "  Decode:       " << (currentMetrics_.avgDecodeTimeUs / 1000.0) << " ms\n";
    ss << "  Render:       " << (currentMetrics_.avgRenderTimeUs / 1000.0) << " ms\n";
    ss << "\n--- Bandwidth ---\n";
    ss << "  Avg Frame:    " << (currentMetrics_.avgFrameSizeBytes / 1024.0) << " KB\n";
    ss << "  Bitrate:      " << currentMetrics_.avgBitrateKbps << " kbps\n";
    ss << "\n--- Counters ---\n";
    ss << "  Captured:     " << currentMetrics_.totalFramesCaptured << "\n";
    ss << "  Encoded:      " << currentMetrics_.totalFramesEncoded << "\n";
    ss << "  Dropped:      " << currentMetrics_.totalFramesDropped << "\n";
    ss << "  Total Data:   " << (currentMetrics_.totalBytesTransferred / (1024.0 * 1024.0)) << " MB\n";
    
    return ss.str();
}

void MetricsCollector::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    captureTimesUs_.clear();
    colorConvertTimesUs_.clear();
    encodeTimesUs_.clear();
    networkTimesUs_.clear();
    decodeTimesUs_.clear();
    renderTimesUs_.clear();
    frameSizes_.clear();
    frameTimestamps_.clear();
    
    currentMetrics_ = PipelineMetrics{};
}

} // namespace vic::metrics
