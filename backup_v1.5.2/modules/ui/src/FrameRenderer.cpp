#include "FrameRenderer.h"
#include "Logger.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

namespace vic::ui {

using Microsoft::WRL::ComPtr;

namespace {

// Simple vertex shader para full-screen quad
const char* kVertexShaderSource = R"(
struct VS_INPUT {
    float2 pos : POSITION;
    float2 tex : TEXCOORD;
};

struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    output.pos = float4(input.pos, 0.0f, 1.0f);
    output.tex = input.tex;
    return output;
}
)";

// Simple pixel shader para mostrar textura BGRA
const char* kPixelShaderSource = R"(
Texture2D frameTexture : register(t0);
SamplerState frameSampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD;
};

float4 main(PS_INPUT input) : SV_TARGET {
    return frameTexture.Sample(frameSampler, input.tex);
}
)";

struct Vertex {
    float x, y;   // Position
    float u, v;   // Texture coords
};

// Full-screen quad vertices
const Vertex kQuadVertices[] = {
    { -1.0f,  1.0f, 0.0f, 0.0f },  // Top-left
    {  1.0f,  1.0f, 1.0f, 0.0f },  // Top-right
    { -1.0f, -1.0f, 0.0f, 1.0f },  // Bottom-left
    {  1.0f, -1.0f, 1.0f, 1.0f },  // Bottom-right
};

/// Direct3D 11 renderer - hardware accelerated
class D3D11Renderer final : public FrameRenderer {
public:
    D3D11Renderer() = default;
    ~D3D11Renderer() override { Shutdown(); }

    bool Initialize(HWND hwnd) override {
        hwnd_ = hwnd;

        if (!CreateDeviceAndSwapChain()) {
            return false;
        }

        if (!CreateRenderTargetView()) {
            return false;
        }

        if (!CreateShaders()) {
            return false;
        }

        if (!CreateVertexBuffer()) {
            return false;
        }

        if (!CreateSamplerState()) {
            return false;
        }

        RECT rect;
        GetClientRect(hwnd_, &rect);
        Resize(rect.right - rect.left, rect.bottom - rect.top);

        logging::global().log(logging::Logger::Level::Info, 
            "D3D11Renderer: Initialized successfully");
        
        initialized_ = true;
        return true;
    }

    void Resize(uint32_t width, uint32_t height) override {
        if (width == 0 || height == 0) return;
        if (width == viewportWidth_ && height == viewportHeight_) return;

        viewportWidth_ = width;
        viewportHeight_ = height;

        // Release old render target
        renderTargetView_.Reset();

        // Resize swap chain
        HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            logging::global().log(logging::Logger::Level::Warning, 
                "D3D11Renderer: Failed to resize swap chain");
            return;
        }

        // Recreate render target view
        CreateRenderTargetView();

        // Update viewport
        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(width);
        vp.Height = static_cast<float>(height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &vp);
    }

    void RenderFrame(const vic::capture::DesktopFrame& frame) override {
        if (!initialized_) return;

        // Update or create texture if dimensions changed
        if (frame.width != textureWidth_ || frame.height != textureHeight_) {
            CreateFrameTexture(frame.width, frame.height);
        }

        // Update texture data
        if (frameTexture_) {
            D3D11_MAPPED_SUBRESOURCE mapped;
            HRESULT hr = context_->Map(stagingTexture_.Get(), 0, D3D11_MAP_WRITE, 0, &mapped);
            if (SUCCEEDED(hr)) {
                const uint32_t srcPitch = frame.width * 4;
                const uint8_t* src = frame.bgraData.data();
                uint8_t* dst = static_cast<uint8_t*>(mapped.pData);

                for (uint32_t y = 0; y < frame.height; ++y) {
                    memcpy(dst + y * mapped.RowPitch, src + y * srcPitch, srcPitch);
                }

                context_->Unmap(stagingTexture_.Get(), 0);
                context_->CopyResource(frameTexture_.Get(), stagingTexture_.Get());
            }
        }

        // Set render target
        context_->OMSetRenderTargets(1, renderTargetView_.GetAddressOf(), nullptr);

        // Clear to black
        float clearColor[] = { 0.0f, 0.0f, 0.0f, 1.0f };
        context_->ClearRenderTargetView(renderTargetView_.Get(), clearColor);

        // Set shaders
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);

        // Set texture and sampler
        context_->PSSetShaderResources(0, 1, frameShaderView_.GetAddressOf());
        context_->PSSetSamplers(0, 1, samplerState_.GetAddressOf());

        // Set vertex buffer
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        context_->IASetVertexBuffers(0, 1, vertexBuffer_.GetAddressOf(), &stride, &offset);
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        // Draw
        context_->Draw(4, 0);
    }

    void Present() override {
        if (swapChain_) {
            // Present with vsync disabled for lowest latency
            swapChain_->Present(0, 0);
        }
    }

    void Shutdown() override {
        frameShaderView_.Reset();
        frameTexture_.Reset();
        stagingTexture_.Reset();
        samplerState_.Reset();
        vertexBuffer_.Reset();
        inputLayout_.Reset();
        pixelShader_.Reset();
        vertexShader_.Reset();
        renderTargetView_.Reset();
        swapChain_.Reset();
        context_.Reset();
        device_.Reset();
        initialized_ = false;
    }

    const char* GetName() const override { return "Direct3D 11"; }
    bool IsValid() const override { return initialized_; }

private:
    bool CreateDeviceAndSwapChain() {
        DXGI_SWAP_CHAIN_DESC scd = {};
        scd.BufferCount = 2;
        scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.BufferDesc.RefreshRate.Numerator = 0;
        scd.BufferDesc.RefreshRate.Denominator = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow = hwnd_;
        scd.SampleDesc.Count = 1;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL featureLevel;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            featureLevels,
            2,
            D3D11_SDK_VERSION,
            &scd,
            swapChain_.GetAddressOf(),
            device_.GetAddressOf(),
            &featureLevel,
            context_.GetAddressOf()
        );

        if (FAILED(hr)) {
            // Try without tearing flag
            scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            scd.Flags = 0;
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                featureLevels, 2, D3D11_SDK_VERSION, &scd,
                swapChain_.GetAddressOf(), device_.GetAddressOf(),
                &featureLevel, context_.GetAddressOf()
            );
        }

        if (FAILED(hr)) {
            logging::global().log(logging::Logger::Level::Error, 
                "D3D11Renderer: Failed to create device and swap chain");
            return false;
        }

        return true;
    }

    bool CreateRenderTargetView() {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        if (FAILED(hr)) return false;

        hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, renderTargetView_.GetAddressOf());
        return SUCCEEDED(hr);
    }

    bool CreateShaders() {
        // Compile vertex shader
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = D3DCompile(
            kVertexShaderSource, strlen(kVertexShaderSource),
            "VS", nullptr, nullptr, "main", "vs_4_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            vsBlob.GetAddressOf(), errorBlob.GetAddressOf()
        );
        
        if (FAILED(hr)) {
            if (errorBlob) {
                logging::global().log(logging::Logger::Level::Error, 
                    std::string("VS compile error: ") + 
                    static_cast<char*>(errorBlob->GetBufferPointer()));
            }
            return false;
        }

        hr = device_->CreateVertexShader(
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
            nullptr, vertexShader_.GetAddressOf()
        );
        if (FAILED(hr)) return false;

        // Create input layout
        D3D11_INPUT_ELEMENT_DESC layoutDesc[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };

        hr = device_->CreateInputLayout(
            layoutDesc, 2,
            vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
            inputLayout_.GetAddressOf()
        );
        if (FAILED(hr)) return false;

        // Compile pixel shader
        ComPtr<ID3DBlob> psBlob;
        hr = D3DCompile(
            kPixelShaderSource, strlen(kPixelShaderSource),
            "PS", nullptr, nullptr, "main", "ps_4_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            psBlob.GetAddressOf(), errorBlob.ReleaseAndGetAddressOf()
        );
        
        if (FAILED(hr)) {
            if (errorBlob) {
                logging::global().log(logging::Logger::Level::Error, 
                    std::string("PS compile error: ") + 
                    static_cast<char*>(errorBlob->GetBufferPointer()));
            }
            return false;
        }

        hr = device_->CreatePixelShader(
            psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
            nullptr, pixelShader_.GetAddressOf()
        );
        return SUCCEEDED(hr);
    }

    bool CreateVertexBuffer() {
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.ByteWidth = sizeof(kQuadVertices);
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = kQuadVertices;

        HRESULT hr = device_->CreateBuffer(&bd, &initData, vertexBuffer_.GetAddressOf());
        return SUCCEEDED(hr);
    }

    bool CreateSamplerState() {
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;  // Bilinear filtering
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MinLOD = 0;
        sd.MaxLOD = D3D11_FLOAT32_MAX;

        HRESULT hr = device_->CreateSamplerState(&sd, samplerState_.GetAddressOf());
        return SUCCEEDED(hr);
    }

    bool CreateFrameTexture(uint32_t width, uint32_t height) {
        frameTexture_.Reset();
        stagingTexture_.Reset();
        frameShaderView_.Reset();

        textureWidth_ = width;
        textureHeight_ = height;

        // Create main texture (GPU)
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device_->CreateTexture2D(&td, nullptr, frameTexture_.GetAddressOf());
        if (FAILED(hr)) return false;

        // Create staging texture (CPU write)
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        hr = device_->CreateTexture2D(&td, nullptr, stagingTexture_.GetAddressOf());
        if (FAILED(hr)) return false;

        // Create shader resource view
        D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
        srvd.Format = td.Format;
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;

        hr = device_->CreateShaderResourceView(frameTexture_.Get(), &srvd, frameShaderView_.GetAddressOf());
        return SUCCEEDED(hr);
    }

    HWND hwnd_ = nullptr;
    bool initialized_ = false;
    uint32_t viewportWidth_ = 0;
    uint32_t viewportHeight_ = 0;
    uint32_t textureWidth_ = 0;
    uint32_t textureHeight_ = 0;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swapChain_;
    ComPtr<ID3D11RenderTargetView> renderTargetView_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11SamplerState> samplerState_;
    ComPtr<ID3D11Texture2D> frameTexture_;
    ComPtr<ID3D11Texture2D> stagingTexture_;
    ComPtr<ID3D11ShaderResourceView> frameShaderView_;
};

/// GDI fallback renderer - always available
class GdiRenderer final : public FrameRenderer {
public:
    bool Initialize(HWND hwnd) override {
        hwnd_ = hwnd;
        initialized_ = true;
        logging::global().log(logging::Logger::Level::Info, 
            "GdiRenderer: Initialized (fallback)");
        return true;
    }

    void Resize(uint32_t, uint32_t) override {
        // GDI handles resize automatically
    }

    void RenderFrame(const vic::capture::DesktopFrame& frame) override {
        if (!initialized_ || frame.bgraData.empty()) return;

        HDC hdc = GetDC(hwnd_);
        if (!hdc) return;

        RECT rect;
        GetClientRect(hwnd_, &rect);

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(frame.width);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(frame.height);  // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        StretchDIBits(
            hdc,
            0, 0, rect.right, rect.bottom,
            0, 0, frame.width, frame.height,
            frame.bgraData.data(),
            &bmi,
            DIB_RGB_COLORS,
            SRCCOPY
        );

        ReleaseDC(hwnd_, hdc);
    }

    void Present() override {
        // GDI doesn't need explicit present
    }

    void Shutdown() override {
        initialized_ = false;
    }

    const char* GetName() const override { return "GDI"; }
    bool IsValid() const override { return initialized_; }

private:
    HWND hwnd_ = nullptr;
    bool initialized_ = false;
};

} // anonymous namespace

std::unique_ptr<FrameRenderer> CreateD3D11Renderer() {
    return std::make_unique<D3D11Renderer>();
}

std::unique_ptr<FrameRenderer> CreateGdiRenderer() {
    return std::make_unique<GdiRenderer>();
}

std::unique_ptr<FrameRenderer> CreateBestRenderer() {
    // Try D3D11 first
    auto d3d11 = CreateD3D11Renderer();
    // Note: actual initialization happens when Initialize() is called
    // We return D3D11 renderer and let it fall back to GDI if init fails
    return d3d11;
}

} // namespace vic::ui
