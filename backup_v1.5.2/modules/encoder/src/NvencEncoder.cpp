// NVENC Hardware Encoder - Full Implementation
// Uses NVIDIA Video Codec SDK API dynamically loaded
#include "NvencEncoder.h"
#include "nvenc_api.h"
#include "VideoEncoder.h"
#include "ColorConvert.h"
#include "Logger.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace vic::encoder {

using namespace nvenc;
using Microsoft::WRL::ComPtr;

namespace {

// Global state for NVENC library
struct NvencLibrary {
    HMODULE module = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api = {};
    bool initialized = false;
    bool available = false;
    std::string errorMessage;
};

static NvencLibrary g_nvenc = {};

bool initNvencLibrary() {
    if (g_nvenc.initialized) {
        return g_nvenc.available;
    }
    g_nvenc.initialized = true;

    // Load NVENC DLL
#ifdef _WIN64
    g_nvenc.module = LoadLibraryW(L"nvEncodeAPI64.dll");
#else
    g_nvenc.module = LoadLibraryW(L"nvEncodeAPI.dll");
#endif

    if (!g_nvenc.module) {
        g_nvenc.errorMessage = "nvEncodeAPI DLL not found - NVIDIA driver not installed or too old";
        logging::global().log(logging::Logger::Level::Info, "NVENC: " + g_nvenc.errorMessage);
        return false;
    }

    // Get API version
    auto nvEncodeAPIGetMaxSupportedVersion = reinterpret_cast<PNVENCODEAPIGETMAXSUPPORTEDVERSION>(
        GetProcAddress(g_nvenc.module, "NvEncodeAPIGetMaxSupportedVersion"));
    
    if (nvEncodeAPIGetMaxSupportedVersion) {
        uint32_t maxVersion = 0;
        if (nvEncodeAPIGetMaxSupportedVersion(&maxVersion) == NV_ENC_SUCCESS) {
            uint32_t major = (maxVersion >> 4) & 0xF;
            uint32_t minor = maxVersion & 0xF;
            logging::global().log(logging::Logger::Level::Info, 
                "NVENC: Max supported API version: " + std::to_string(major) + "." + std::to_string(minor));
        }
    }

    // Get function list
    auto nvEncodeAPICreateInstance = reinterpret_cast<PNVENCODEAPICREATEINSTANCE>(
        GetProcAddress(g_nvenc.module, "NvEncodeAPICreateInstance"));
    
    if (!nvEncodeAPICreateInstance) {
        g_nvenc.errorMessage = "NvEncodeAPICreateInstance not found in DLL";
        FreeLibrary(g_nvenc.module);
        g_nvenc.module = nullptr;
        return false;
    }

    // Initialize function list
    memset(&g_nvenc.api, 0, sizeof(g_nvenc.api));
    g_nvenc.api.version = NV_ENCODE_API_FUNCTION_LIST_VER;

    NVENCSTATUS status = nvEncodeAPICreateInstance(&g_nvenc.api);
    if (status != NV_ENC_SUCCESS) {
        g_nvenc.errorMessage = "NvEncodeAPICreateInstance failed with status " + std::to_string(status);
        FreeLibrary(g_nvenc.module);
        g_nvenc.module = nullptr;
        return false;
    }

    g_nvenc.available = true;
    logging::global().log(logging::Logger::Level::Info, "NVENC: API loaded successfully");
    return true;
}

// NVENC Encoder Implementation
class NvencEncoderImpl final : public VideoEncoder {
public:
    explicit NvencEncoderImpl(const NvencConfig& config)
        : config_(config) {
        colorConverter_ = createColorConverter();
    }

    ~NvencEncoderImpl() override {
        Shutdown();
    }

    bool Configure(uint32_t width, uint32_t height, uint32_t targetBitrateKbps) override {
        if (width == 0 || height == 0) {
            return false;
        }

        if (initialized_ && width == width_ && height == height_) {
            // Just update bitrate if dimensions unchanged
            config_.targetBitrateKbps = targetBitrateKbps;
            return true;
        }

        Shutdown();

        width_ = width;
        height_ = height;
        config_.targetBitrateKbps = targetBitrateKbps > 0 ? targetBitrateKbps : config_.targetBitrateKbps;

        // Create D3D11 device
        if (!CreateD3D11Device()) {
            logging::global().log(logging::Logger::Level::Error, "NVENC: Failed to create D3D11 device");
            return false;
        }

        // Open NVENC session
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {};
        sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        sessionParams.device = device_.Get();
        sessionParams.apiVersion = NVENCAPI_VERSION;

        NVENCSTATUS status = g_nvenc.api.nvEncOpenEncodeSessionEx(&sessionParams, &encoder_);
        if (status != NV_ENC_SUCCESS) {
            logging::global().log(logging::Logger::Level::Error, 
                "NVENC: nvEncOpenEncodeSessionEx failed: " + std::to_string(status));
            return false;
        }

        // Get preset config
        NV_ENC_PRESET_CONFIG presetConfig = {};
        presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
        presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;

        // Use P1 for lowest latency or P4 for balanced
        GUID presetGUID = config_.lowLatencyMode ? NV_ENC_PRESET_P1_GUID : NV_ENC_PRESET_P4_GUID;
        NV_ENC_TUNING_INFO tuning = config_.lowLatencyMode ? 
            NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY : NV_ENC_TUNING_INFO_LOW_LATENCY;

        status = g_nvenc.api.nvEncGetEncodePresetConfigEx(
            encoder_, NV_ENC_CODEC_H264_GUID, presetGUID, tuning, &presetConfig);
        
        if (status != NV_ENC_SUCCESS) {
            logging::global().log(logging::Logger::Level::Error, 
                "NVENC: nvEncGetEncodePresetConfigEx failed: " + std::to_string(status));
            g_nvenc.api.nvEncDestroyEncoder(encoder_);
            encoder_ = nullptr;
            return false;
        }

        // Configure encoding parameters
        NV_ENC_CONFIG encodeConfig = presetConfig.presetCfg;
        
        // Rate control
        encodeConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ;
        encodeConfig.rcParams.averageBitRate = config_.targetBitrateKbps * 1000;
        encodeConfig.rcParams.maxBitRate = config_.targetBitrateKbps * 1200; // 20% headroom
        encodeConfig.rcParams.vbvBufferSize = config_.targetBitrateKbps * 1000 / 30; // ~1 frame buffer
        encodeConfig.rcParams.vbvInitialDelay = encodeConfig.rcParams.vbvBufferSize;
        encodeConfig.rcParams.zeroReorderDelay = 1;
        encodeConfig.rcParams.enableAQ = 1;
        
        // H.264 specific
        encodeConfig.encodeCodecConfig.h264Config.idrPeriod = config_.gopLength;
        encodeConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
        encodeConfig.encodeCodecConfig.h264Config.sliceMode = 0;
        encodeConfig.encodeCodecConfig.h264Config.sliceModeData = 0;
        
        // GOP structure
        encodeConfig.gopLength = config_.gopLength;
        encodeConfig.frameIntervalP = 1; // No B-frames for low latency

        // Initialize encoder
        NV_ENC_INITIALIZE_PARAMS initParams = {};
        initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
        initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
        initParams.presetGUID = presetGUID;
        initParams.encodeWidth = width;
        initParams.encodeHeight = height;
        initParams.darWidth = width;
        initParams.darHeight = height;
        initParams.frameRateNum = 60; // Fixed for now
        initParams.frameRateDen = 1;
        initParams.enablePTD = 1;  // Picture type decision by encoder
        initParams.encodeConfig = &encodeConfig;
        initParams.tuningInfo = tuning;

        status = g_nvenc.api.nvEncInitializeEncoder(encoder_, &initParams);
        if (status != NV_ENC_SUCCESS) {
            logging::global().log(logging::Logger::Level::Error, 
                "NVENC: nvEncInitializeEncoder failed: " + std::to_string(status));
            g_nvenc.api.nvEncDestroyEncoder(encoder_);
            encoder_ = nullptr;
            return false;
        }

        // Create input buffer (NV12 format)
        NV_ENC_CREATE_INPUT_BUFFER createInputParams = {};
        createInputParams.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
        createInputParams.width = width;
        createInputParams.height = height;
        createInputParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;

        status = g_nvenc.api.nvEncCreateInputBuffer(encoder_, &createInputParams);
        if (status != NV_ENC_SUCCESS) {
            logging::global().log(logging::Logger::Level::Error, 
                "NVENC: nvEncCreateInputBuffer failed: " + std::to_string(status));
            g_nvenc.api.nvEncDestroyEncoder(encoder_);
            encoder_ = nullptr;
            return false;
        }
        inputBuffer_ = createInputParams.inputBuffer;

        // Create output bitstream buffer
        NV_ENC_CREATE_BITSTREAM_BUFFER createBitstreamParams = {};
        createBitstreamParams.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

        status = g_nvenc.api.nvEncCreateBitstreamBuffer(encoder_, &createBitstreamParams);
        if (status != NV_ENC_SUCCESS) {
            logging::global().log(logging::Logger::Level::Error, 
                "NVENC: nvEncCreateBitstreamBuffer failed: " + std::to_string(status));
            g_nvenc.api.nvEncDestroyInputBuffer(encoder_, inputBuffer_);
            g_nvenc.api.nvEncDestroyEncoder(encoder_);
            encoder_ = nullptr;
            inputBuffer_ = nullptr;
            return false;
        }
        outputBuffer_ = createBitstreamParams.bitstreamBuffer;

        // Allocate NV12 conversion buffer
        nv12Buffer_.resize(width * height * 3 / 2);

        initialized_ = true;
        frameIndex_ = 0;

        logging::global().log(logging::Logger::Level::Info, 
            "NVENC: Initialized H.264 encoder " + std::to_string(width) + "x" + std::to_string(height) +
            " @ " + std::to_string(config_.targetBitrateKbps) + " kbps");

        return true;
    }

    std::optional<EncodedFrame> EncodeFrame(const vic::capture::DesktopFrame& frame) override {
        if (!initialized_) {
            if (!Configure(frame.width, frame.height, config_.targetBitrateKbps)) {
                return std::nullopt;
            }
        }

        if (frame.width != width_ || frame.height != height_) {
            if (!Configure(frame.width, frame.height, config_.targetBitrateKbps)) {
                return std::nullopt;
            }
        }

        auto startTime = std::chrono::steady_clock::now();

        // Convert BGRA to NV12
        // First convert to I420, then interleave UV planes
        std::vector<uint8_t> yPlane(width_ * height_);
        std::vector<uint8_t> uPlane(width_ * height_ / 4);
        std::vector<uint8_t> vPlane(width_ * height_ / 4);

        colorConverter_->BGRAToI420(
            frame.bgraData.data(), width_ * 4,
            yPlane.data(), width_,
            uPlane.data(), width_ / 2,
            vPlane.data(), width_ / 2,
            width_, height_
        );

        // Copy Y plane
        memcpy(nv12Buffer_.data(), yPlane.data(), width_ * height_);

        // Interleave U and V into NV12 UV plane
        uint8_t* uvDst = nv12Buffer_.data() + width_ * height_;
        const uint32_t uvWidth = width_ / 2;
        const uint32_t uvHeight = height_ / 2;
        for (uint32_t y = 0; y < uvHeight; ++y) {
            for (uint32_t x = 0; x < uvWidth; ++x) {
                uvDst[y * width_ + x * 2 + 0] = uPlane[y * uvWidth + x];
                uvDst[y * width_ + x * 2 + 1] = vPlane[y * uvWidth + x];
            }
        }

        // Lock input buffer and copy NV12 data
        NV_ENC_LOCK_INPUT_BUFFER lockInputParams = {};
        lockInputParams.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lockInputParams.inputBuffer = inputBuffer_;

        NVENCSTATUS status = g_nvenc.api.nvEncLockInputBuffer(encoder_, &lockInputParams);
        if (status != NV_ENC_SUCCESS) {
            logging::global().log(logging::Logger::Level::Error, 
                "NVENC: nvEncLockInputBuffer failed: " + std::to_string(status));
            return std::nullopt;
        }

        // Copy NV12 data to input buffer
        uint8_t* dst = static_cast<uint8_t*>(lockInputParams.bufferDataPtr);
        const uint32_t pitch = lockInputParams.pitch;

        // Copy Y plane with pitch
        for (uint32_t y = 0; y < height_; ++y) {
            memcpy(dst + y * pitch, nv12Buffer_.data() + y * width_, width_);
        }

        // Copy UV plane with pitch
        uint8_t* uvSrc = nv12Buffer_.data() + width_ * height_;
        uint8_t* uvDstBuf = dst + pitch * height_;
        for (uint32_t y = 0; y < height_ / 2; ++y) {
            memcpy(uvDstBuf + y * pitch, uvSrc + y * width_, width_);
        }

        g_nvenc.api.nvEncUnlockInputBuffer(encoder_, inputBuffer_);

        // Encode frame
        NV_ENC_PIC_PARAMS picParams = {};
        picParams.version = NV_ENC_PIC_PARAMS_VER;
        picParams.inputWidth = width_;
        picParams.inputHeight = height_;
        picParams.inputPitch = pitch;
        picParams.inputBuffer = inputBuffer_;
        picParams.outputBitstream = outputBuffer_;
        picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
        picParams.pictureStruct = 1;  // Frame
        picParams.inputTimeStamp = frameIndex_;
        picParams.frameIdx = static_cast<uint32_t>(frameIndex_);

        // Request IDR frame periodically or on first frame
        if (frameIndex_ == 0 || (frameIndex_ % config_.gopLength == 0)) {
            picParams.encodePicFlags = 0x04;  // NV_ENC_PIC_FLAG_FORCEIDR
        }

        status = g_nvenc.api.nvEncEncodePicture(encoder_, &picParams);
        if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
            logging::global().log(logging::Logger::Level::Error, 
                "NVENC: nvEncEncodePicture failed: " + std::to_string(status));
            return std::nullopt;
        }

        // Lock output bitstream
        NV_ENC_LOCK_BITSTREAM lockBitstreamParams = {};
        lockBitstreamParams.version = NV_ENC_LOCK_BITSTREAM_VER;
        lockBitstreamParams.outputBitstream = outputBuffer_;

        status = g_nvenc.api.nvEncLockBitstream(encoder_, &lockBitstreamParams);
        if (status != NV_ENC_SUCCESS) {
            logging::global().log(logging::Logger::Level::Error, 
                "NVENC: nvEncLockBitstream failed: " + std::to_string(status));
            return std::nullopt;
        }

        // Copy encoded data
        EncodedFrame result;
        result.payload.resize(lockBitstreamParams.bitstreamSizeInBytes);
        memcpy(result.payload.data(), lockBitstreamParams.bitstreamBufferPtr, 
               lockBitstreamParams.bitstreamSizeInBytes);
        result.keyFrame = (lockBitstreamParams.pictureType == NV_ENC_PIC_TYPE_IDR ||
                          lockBitstreamParams.pictureType == NV_ENC_PIC_TYPE_I);
        result.timestamp = frame.timestamp;
        result.width = width_;
        result.height = height_;

        g_nvenc.api.nvEncUnlockBitstream(encoder_, outputBuffer_);

        auto endTime = std::chrono::steady_clock::now();
        auto encodeTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

        logging::global().log(logging::Logger::Level::Debug, 
            "NVENC: Encoded frame " + std::to_string(frameIndex_) + 
            " size=" + std::to_string(result.payload.size()) +
            " (" + (result.keyFrame ? "IDR" : "P") + ")" +
            " time=" + std::to_string(encodeTimeMs) + "ms");

        frameIndex_++;
        return result;
    }

    std::vector<uint8_t> Flush() override {
        if (!initialized_ || !encoder_) {
            return {};
        }

        // Send EOS
        NV_ENC_PIC_PARAMS picParams = {};
        picParams.version = NV_ENC_PIC_PARAMS_VER;
        picParams.encodePicFlags = 0x01;  // NV_ENC_PIC_FLAG_EOS

        g_nvenc.api.nvEncEncodePicture(encoder_, &picParams);

        return {};
    }

private:
    bool CreateD3D11Device() {
        D3D_FEATURE_LEVEL featureLevels[] = { 
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0 
        };
        D3D_FEATURE_LEVEL featureLevel;

        // Try to find NVIDIA adapter
        ComPtr<IDXGIFactory1> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            return false;
        }

        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIAdapter1> nvidiaAdapter;
        
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            
            // Check for NVIDIA vendor ID
            if (desc.VendorId == 0x10DE) {
                nvidiaAdapter = adapter;
                char adapterName[256];
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapterName, 256, nullptr, nullptr);
                logging::global().log(logging::Logger::Level::Info, 
                    "NVENC: Using GPU: " + std::string(adapterName));
                break;
            }
            adapter.Reset();
        }

        IDXGIAdapter* selectedAdapter = nvidiaAdapter ? nvidiaAdapter.Get() : nullptr;

        hr = D3D11CreateDevice(
            selectedAdapter,
            selectedAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            2,
            D3D11_SDK_VERSION,
            device_.GetAddressOf(),
            &featureLevel,
            context_.GetAddressOf()
        );

        return SUCCEEDED(hr);
    }

    void Shutdown() {
        if (encoder_) {
            if (outputBuffer_) {
                g_nvenc.api.nvEncDestroyBitstreamBuffer(encoder_, outputBuffer_);
                outputBuffer_ = nullptr;
            }
            if (inputBuffer_) {
                g_nvenc.api.nvEncDestroyInputBuffer(encoder_, inputBuffer_);
                inputBuffer_ = nullptr;
            }
            g_nvenc.api.nvEncDestroyEncoder(encoder_);
            encoder_ = nullptr;
        }

        context_.Reset();
        device_.Reset();
        nv12Buffer_.clear();
        
        initialized_ = false;
        width_ = height_ = 0;
        frameIndex_ = 0;
    }

    NvencConfig config_;
    bool initialized_ = false;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint64_t frameIndex_ = 0;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;

    void* encoder_ = nullptr;
    void* inputBuffer_ = nullptr;
    void* outputBuffer_ = nullptr;

    std::vector<uint8_t> nv12Buffer_;
    std::unique_ptr<ColorConverter> colorConverter_;
};

} // anonymous namespace

bool isNvencAvailable() {
    return initNvencLibrary();
}

std::string getNvencInfo() {
    if (!initNvencLibrary()) {
        return "NVENC: Not available - " + g_nvenc.errorMessage;
    }
    return "NVENC: Available (H.264 hardware encoding)";
}

std::unique_ptr<VideoEncoder> createNvencEncoder(ID3D11Device* /*device*/) {
    return createNvencEncoder(NvencConfig{}, nullptr);
}

std::unique_ptr<VideoEncoder> createNvencEncoder(const NvencConfig& config, ID3D11Device* /*device*/) {
    if (!isNvencAvailable()) {
        logging::global().log(logging::Logger::Level::Warning, 
            "NVENC not available: " + g_nvenc.errorMessage);
        return nullptr;
    }

    return std::make_unique<NvencEncoderImpl>(config);
}

std::unique_ptr<VideoEncoder> createBestEncoder() {
    // Try NVENC first
    if (isNvencAvailable()) {
        NvencConfig config;
        config.lowLatencyMode = true;
        config.targetBitrateKbps = 8000;  // Higher default for H.264
        config.gopLength = 60;

        auto nvenc = createNvencEncoder(config, nullptr);
        if (nvenc) {
            logging::global().log(logging::Logger::Level::Info, 
                "Using NVENC H.264 hardware encoder");
            return nvenc;
        }
    }

    // Fallback to VP8
    logging::global().log(logging::Logger::Level::Info, 
        "Using VP8 software encoder (NVENC not available)");
    return createVp8Encoder();
}

} // namespace vic::encoder
