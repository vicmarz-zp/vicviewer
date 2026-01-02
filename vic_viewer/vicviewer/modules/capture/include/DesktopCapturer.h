#pragma once

#include "DesktopFrame.h"

#include <memory>
#include <optional>
#include <variant>

struct IDXGIOutputDuplication;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGIOutput1;

namespace vic::capture {

class GdiCapturer; // Forward declaration

class DesktopCapturer {
public:
    DesktopCapturer();
    ~DesktopCapturer();

    DesktopCapturer(const DesktopCapturer&) = delete;
    DesktopCapturer& operator=(const DesktopCapturer&) = delete;
    DesktopCapturer(DesktopCapturer&&) noexcept;
    DesktopCapturer& operator=(DesktopCapturer&&) noexcept;

    bool initialize();
    std::unique_ptr<DesktopFrame> captureFrame();

private:
    struct DxgiCapturer; // Renamed from Impl
    
    std::variant<std::unique_ptr<DxgiCapturer>, std::unique_ptr<GdiCapturer>> m_capturer;
};

} // namespace vic::capture
