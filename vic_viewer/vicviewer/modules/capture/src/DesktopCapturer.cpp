#include "DesktopCapturer.h"
#include "GdiCapturer.h" // Incluir la nueva clase
#include "Logger.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace vic::capture {

namespace {
    void logIfFailed(const char* message, HRESULT hr) {
        if (FAILED(hr)) {
            logging::global().log(logging::Logger::Level::Error,
                std::string(message) + ": hr=0x" + std::to_string(hr));
        }
    }
} // namespace

struct DesktopCapturer::DxgiCapturer {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTexture;
    DXGI_OUTDUPL_DESC duplicationDesc{};
    uint32_t width = 0;
    uint32_t height = 0;

    bool initialize() {
        cleanup();

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0};

        D3D_FEATURE_LEVEL capturedLevel{};
        HRESULT hr = D3D11CreateDevice(nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            device.GetAddressOf(),
            &capturedLevel,
            context.GetAddressOf());

        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr,
                D3D_DRIVER_TYPE_WARP,
                nullptr,
                flags,
                levels,
                static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION,
                device.GetAddressOf(),
                &capturedLevel,
                context.GetAddressOf());
        }

        if (FAILED(hr)) {
            logIfFailed("DXGI: Failed to create D3D11 device", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        hr = device.As(&dxgiDevice);
        if (FAILED(hr)) {
            logIfFailed("DXGI: Failed to query IDXGIDevice", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
        if (FAILED(hr)) {
            logIfFailed("DXGI: Failed to get adapter", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        hr = adapter->EnumOutputs(0, output.GetAddressOf());
        if (FAILED(hr)) {
            logIfFailed("DXGI: Failed to enumerate adapter outputs", hr);
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
        hr = output.As(&output1);
        if (FAILED(hr)) {
            logIfFailed("DXGI: Output does not support IDXGIOutput1", hr);
            return false;
        }

        hr = output1->DuplicateOutput(device.Get(), duplication.GetAddressOf());
        if (FAILED(hr)) {
            logIfFailed("DXGI: DuplicateOutput failed", hr);
            logging::global().log(logging::Logger::Level::Error,
                "DXGI Desktop Duplication fallo - probablemente permisos o RDP");
            return false; // Fallar para que se intente GDI
        }

        duplication->GetDesc(&duplicationDesc);
        width = duplicationDesc.ModeDesc.Width;
        height = duplicationDesc.ModeDesc.Height;

        createStagingTexture();
        return stagingTexture != nullptr;
    }

    void createStagingTexture() {
        if (!device) {
            return;
        }

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;

        stagingTexture.Reset();
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, stagingTexture.GetAddressOf());
        if (FAILED(hr)) {
            logIfFailed("DXGI: Failed to create staging texture", hr);
        }
    }

    void cleanup() {
        stagingTexture.Reset();
        duplication.Reset();
        context.Reset();
        device.Reset();
        duplicationDesc = {};
        width = 0;
        height = 0;
    }

    bool ensureInitialized() {
        if (duplication) {
            return true;
        }
        return initialize();
    }

    std::unique_ptr<DesktopFrame> captureFrame() {
        if (!ensureInitialized()) {
            return nullptr;
        }

        Microsoft::WRL::ComPtr<IDXGIResource> resource;
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        // Timeout reducido a 16ms para permitir hasta 60 FPS
        // (antes era 50ms que limitaba a ~20 FPS)
        HRESULT hr = duplication->AcquireNextFrame(16, &frameInfo, resource.GetAddressOf());

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return nullptr;
        }
        if (FAILED(hr)) {
            logIfFailed("DXGI: Failed to acquire next frame", hr);
            cleanup();
            initialize();
            return nullptr;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        hr = resource.As(&texture);
        if (FAILED(hr)) {
            duplication->ReleaseFrame();
            return nullptr;
        }

        context->CopyResource(stagingTexture.Get(), texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};;
        hr = context->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            duplication->ReleaseFrame();
            return nullptr;
        }

        // ========== OPTIMIZACIÓN: Buffer reutilizable ==========
        // Reutilizar el buffer BGRA si el tamaño no cambió
        const size_t requiredSize = static_cast<size_t>(width) * height * 4;
        if (bgraBuffer_.size() != requiredSize) {
            bgraBuffer_.resize(requiredSize);
        }

        auto frame = std::make_unique<DesktopFrame>();
        frame->width = width;
        frame->height = height;

        const uint8_t* source = static_cast<const uint8_t*>(mapped.pData);
        const uint32_t sourcePitch = mapped.RowPitch;
        const uint32_t destPitch = width * 4;

        // Copiar línea por línea al buffer reutilizable
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(bgraBuffer_.data() + y * destPitch, source + y * sourcePitch, destPitch);
        }

        context->Unmap(stagingTexture.Get(), 0);
        duplication->ReleaseFrame();

        // Asignar del buffer al frame (esto hace una copia, pero el buffer permanece)
        frame->bgraData.assign(bgraBuffer_.begin(), bgraBuffer_.end());

        frame->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                               .count();
        return frame;
    }
    
    // Buffer BGRA reutilizable - evita allocation por frame
    std::vector<uint8_t> bgraBuffer_;
};

DesktopCapturer::DesktopCapturer() {
    // El constructor ahora está vacío, la lógica se mueve a initialize
}

DesktopCapturer::~DesktopCapturer() = default;
DesktopCapturer::DesktopCapturer(DesktopCapturer&&) noexcept = default;
DesktopCapturer& DesktopCapturer::operator=(DesktopCapturer&&) noexcept = default;

bool DesktopCapturer::initialize() {
    auto dxgi = std::make_unique<DxgiCapturer>();
    if (dxgi->initialize()) {
        logging::global().log(logging::Logger::Level::Info, "DXGI capturer inicializado correctamente");
        m_capturer = std::move(dxgi);
        return true;
    }

    logging::global().log(logging::Logger::Level::Warning,
        "Fallo al inicializar DXGI, intentando con GDI como fallback.");

    auto gdi = std::make_unique<GdiCapturer>();
    if (gdi->initialize()) {
        logging::global().log(logging::Logger::Level::Info, "GDI capturer inicializado como fallback.");
        m_capturer = std::move(gdi);
        return true;
    }

    logging::global().log(logging::Logger::Level::Error,
        "Fallo al inicializar tanto DXGI como GDI. No se puede capturar el escritorio.");
    return false;
}

std::unique_ptr<DesktopFrame> DesktopCapturer::captureFrame() {
    return std::visit(
        [](auto& capturer) -> std::unique_ptr<DesktopFrame> {
            if (capturer) {
                return capturer->captureFrame();
            }
            return nullptr;
        },
        m_capturer);
}

} // namespace vic::capture
