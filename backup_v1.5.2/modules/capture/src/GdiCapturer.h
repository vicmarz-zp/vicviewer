#pragma once

#include "DesktopFrame.h"
#include <memory>
#include <windows.h>

namespace vic::capture {

// Implementación de respaldo para captura de pantalla usando GDI (BitBlt).
// Es más lento que DXGI pero compatible con RDP y entornos virtuales.
class GdiCapturer {
public:
    GdiCapturer();
    ~GdiCapturer();

    bool initialize();
    std::unique_ptr<DesktopFrame> captureFrame();

    int getWidth() const { return m_width; }
    int getHeight() const { return m_height; }

private:
    void cleanup();

    HDC m_hdcScreen{nullptr};
    HDC m_hdcMem{nullptr};
    HBITMAP m_hBitmap{nullptr};
    HGDIOBJ m_hOldBitmap{nullptr};
    int m_width{0};
    int m_height{0};
    std::vector<uint8_t> m_buffer;
};

} // namespace vic::capture
