#include "VideoEncoder.h"
#include "ColorConvert.h"

#include "Logger.h"

#include <vpx/vpx_encoder.h>
#include <vpx/vp8cx.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace vic::encoder {

namespace {

// Optimización para WAN: bitrate adaptativo, menor latencia
// 2500kbps es suficiente para escritorio 1080p con VP8 (texto legible)
constexpr uint32_t kDefaultBitrateKbps = 2500;
constexpr uint32_t kPixelsPerThreadHint = 640u * 360u;
constexpr int kDefaultCpuUsed = 10; // Máximo speed para mínima latencia

uint8_t clampToByte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

class LibvpxEncoder final : public VideoEncoder {
public:
    LibvpxEncoder() = default;
    ~LibvpxEncoder() override {
        Shutdown();
    }

    bool Configure(uint32_t width, uint32_t height, uint32_t targetBitrateKbps) override {
        if (width == 0 || height == 0) {
            logging::global().log(logging::Logger::Level::Error, "Encoder configure received invalid dimensions");
            return false;
        }

        if (initialized_) {
            Shutdown();
        }

        width_ = width;
        height_ = height;
        targetBitrateKbps_ = targetBitrateKbps == 0 ? kDefaultBitrateKbps : targetBitrateKbps;

        const vpx_codec_iface_t* iface = vpx_codec_vp8_cx();
        if (vpx_codec_enc_config_default(iface, &config_, 0) != VPX_CODEC_OK) {
            logging::global().log(logging::Logger::Level::Error, "Failed to get default VP8 encoder config");
            return false;
        }

        config_.g_w = width_;
        config_.g_h = height_;
        config_.g_timebase.num = 1;
        config_.g_timebase.den = 1000; // millisecond timestamps
        config_.rc_target_bitrate = std::max<uint32_t>(1, targetBitrateKbps_);
        config_.g_threads = std::clamp<uint32_t>((width_ * height_) / kPixelsPerThreadHint, 1, 8);
        config_.rc_end_usage = VPX_CBR;
        config_.kf_mode = VPX_KF_AUTO;
        config_.g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT;
        config_.rc_min_quantizer = 2;   // Permite mejor calidad en escenas estáticas
        config_.rc_max_quantizer = 48;  // Permite más compresión cuando sea necesario
        config_.g_lag_in_frames = 0;    // Zero latency - no buffering
        config_.rc_buf_sz = 100;        // Buffer más pequeño para menor latencia
        config_.rc_buf_initial_sz = 50;
        config_.rc_buf_optimal_sz = 75;

        if (vpx_codec_enc_init(&codec_, iface, &config_, 0) != VPX_CODEC_OK) {
            logging::global().log(logging::Logger::Level::Error, "Failed to initialize VP8 encoder context");
            return false;
        }

        // Baseline realtime tuning: modest CPUUSED for low latency desktop capture.
        vpx_codec_control(&codec_, VP8E_SET_CPUUSED, kDefaultCpuUsed);
        vpx_codec_control(&codec_, VP8E_SET_STATIC_THRESHOLD, 0);
        vpx_codec_control(&codec_, VP8E_SET_NOISE_SENSITIVITY, 0);
        vpx_codec_control(&codec_, VP8E_SET_ARNR_MAXFRAMES, 0);
        vpx_codec_control(&codec_, VP8E_SET_ARNR_STRENGTH, 0);
        vpx_codec_control(&codec_, VP8E_SET_ARNR_TYPE, 0);

        const size_t ySize = static_cast<size_t>(width_) * height_;
        const size_t uvWidth = (width_ + 1) / 2;
        const size_t uvHeight = (height_ + 1) / 2;
        const size_t uvSize = uvWidth * uvHeight;

        yuvBuffer_.resize(ySize + 2 * uvSize);
        
        // Inicializar el convertidor de color si no existe
        if (!colorConverter_) {
            colorConverter_ = createColorConverter();
        }

        initialized_ = true;
        return true;
    }

    std::optional<EncodedFrame> EncodeFrame(const vic::capture::DesktopFrame& frame) override {
        auto encodeStart = std::chrono::steady_clock::now();
        
        if (!initialized_) {
            if (!Configure(frame.width, frame.height, targetBitrateKbps_ == 0 ? kDefaultBitrateKbps : targetBitrateKbps_)) {
                return std::nullopt;
            }
        }

        if (frame.bgraData.empty()) {
            logging::global().log(logging::Logger::Level::Warning, "Encoder received empty frame data");
            return std::nullopt;
        }

        if (frame.width != width_ || frame.height != height_) {
            if (!Configure(frame.width, frame.height, targetBitrateKbps_)) {
                return std::nullopt;
            }
        }

        const size_t ySize = static_cast<size_t>(width_) * height_;
        const size_t uvWidth = (width_ + 1) / 2;
        const size_t uvHeight = (height_ + 1) / 2;
        const size_t uvSize = uvWidth * uvHeight;

        if (yuvBuffer_.size() < ySize + 2 * uvSize) {
            yuvBuffer_.resize(ySize + 2 * uvSize);
        }

        auto* yPlane = yuvBuffer_.data();
        auto* uPlane = yPlane + ySize;
        auto* vPlane = uPlane + uvSize;

        // Fase 2: Usar ColorConverter optimizado (SIMD cuando disponible)
        auto colorStart = std::chrono::steady_clock::now();
        
        if (!colorConverter_->BGRAToI420(
                frame.bgraData.data(), static_cast<int>(width_ * 4),
                yPlane, static_cast<int>(width_),
                uPlane, static_cast<int>(uvWidth),
                vPlane, static_cast<int>(uvWidth),
                static_cast<int>(width_), static_cast<int>(height_))) {
            logging::global().log(logging::Logger::Level::Error, "Color conversion failed");
            return std::nullopt;
        }
        
        auto colorEnd = std::chrono::steady_clock::now();
        auto colorMs = std::chrono::duration_cast<std::chrono::microseconds>(colorEnd - colorStart).count();

        vpx_image_t raw{};
        vpx_img_wrap(&raw, VPX_IMG_FMT_I420, width_, height_, 1, yuvBuffer_.data());
        raw.planes[0] = yPlane;
        raw.planes[1] = uPlane;
        raw.planes[2] = vPlane;
        raw.stride[0] = width_;
        raw.stride[1] = raw.stride[2] = uvWidth;

        // Forzar keyframe si: timestamp==0 (primer frame) O si forceKeyframe_ está activo
        vpx_enc_frame_flags_t flags = 0;
        if (frame.timestamp == 0 || forceKeyframe_) {
            flags = VPX_EFLAG_FORCE_KF;
            forceKeyframe_ = false;  // Reset el flag
        }
        
        const vpx_codec_err_t encodeResult = vpx_codec_encode(&codec_, &raw, frame.timestamp, 1, flags, VPX_DL_REALTIME);
        vpx_img_free(&raw);
        if (encodeResult != VPX_CODEC_OK) {
            logging::global().log(logging::Logger::Level::Error, "VP8 encode failed");
            return std::nullopt;
        }

        vpx_codec_iter_t iter = nullptr;
        const vpx_codec_cx_pkt_t* packet = nullptr;
        while ((packet = vpx_codec_get_cx_data(&codec_, &iter)) != nullptr) {
            if (packet->kind == VPX_CODEC_CX_FRAME_PKT) {
                EncodedFrame encoded{};
                encoded.timestamp = frame.timestamp;
                encoded.width = width_;
                encoded.height = height_;
                encoded.keyFrame = (packet->data.frame.flags & VPX_FRAME_IS_KEY) != 0;
                encoded.payload.assign(static_cast<const uint8_t*>(packet->data.frame.buf),
                    static_cast<const uint8_t*>(packet->data.frame.buf) + packet->data.frame.sz);
                logging::global().log(logging::Logger::Level::Debug,
                    std::string("VP8 encoded frame size=") + std::to_string(packet->data.frame.sz) +
                    (encoded.keyFrame ? " (key)" : ""));
                return encoded;
            }
        }

        return std::nullopt;
    }

    std::vector<uint8_t> Flush() override {
        if (!initialized_) {
            return {};
        }

        const vpx_codec_err_t result = vpx_codec_encode(&codec_, nullptr, 0, 0, 0, VPX_DL_REALTIME);
        if (result != VPX_CODEC_OK) {
            return {};
        }

        vpx_codec_iter_t iter = nullptr;
        const vpx_codec_cx_pkt_t* packet = nullptr;
        if ((packet = vpx_codec_get_cx_data(&codec_, &iter)) != nullptr && packet->kind == VPX_CODEC_CX_FRAME_PKT) {
            return {static_cast<const uint8_t*>(packet->data.frame.buf),
                static_cast<const uint8_t*>(packet->data.frame.buf) + packet->data.frame.sz};
        }
        return {};
    }

private:
    void Shutdown() {
        if (initialized_) {
            vpx_codec_destroy(&codec_);
            initialized_ = false;
        }
        width_ = height_ = 0;
        targetBitrateKbps_ = 0;
        yuvBuffer_.clear();
        colorConverter_.reset();
    }

    vpx_codec_ctx_t codec_{};
    vpx_codec_enc_cfg_t config_{};
    bool initialized_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t targetBitrateKbps_ = 0;
    std::vector<uint8_t> yuvBuffer_;
    std::unique_ptr<ColorConverter> colorConverter_;
};

} // namespace

std::unique_ptr<VideoEncoder> createVp8Encoder() {
    return std::make_unique<LibvpxEncoder>();
}

} // namespace vic::encoder
