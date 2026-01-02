#include "GdiCapturer.h"
#include "Logger.h"
#include <chrono>

namespace vic::capture {

GdiCapturer::GdiCapturer() = default;
GdiCapturer::~GdiCapturer() {
    cleanup();
}

void GdiCapturer::cleanup() {
    if (m_hdcMem) {
        SelectObject(m_hdcMem, m_hOldBitmap);
        DeleteObject(m_hBitmap);
        DeleteDC(m_hdcMem);
        m_hdcMem = nullptr;
    }
    if (m_hdcScreen) {
        ReleaseDC(nullptr, m_hdcScreen);
        m_hdcScreen = nullptr;
    }
}

bool GdiCapturer::initialize() {
    cleanup();

    m_hdcScreen = GetDC(nullptr);
    if (!m_hdcScreen) {
        logging::global().log(logging::Logger::Level::Error, "GDI: GetDC(nullptr) falló");
        return false;
    }

    m_width = GetSystemMetrics(SM_CXSCREEN);
    m_height = GetSystemMetrics(SM_CYSCREEN);

    m_hdcMem = CreateCompatibleDC(m_hdcScreen);
    if (!m_hdcMem) {
        logging::global().log(logging::Logger::Level::Error, "GDI: CreateCompatibleDC falló");
        cleanup();
        return false;
    }

    m_hBitmap = CreateCompatibleBitmap(m_hdcScreen, m_width, m_height);
    if (!m_hBitmap) {
        logging::global().log(logging::Logger::Level::Error, "GDI: CreateCompatibleBitmap falló");
        cleanup();
        return false;
    }

    m_hOldBitmap = SelectObject(m_hdcMem, m_hBitmap);
    m_buffer.resize(m_width * m_height * 4);

    logging::global().log(logging::Logger::Level::Info, "GDI Capturer inicializado correctamente");
    return true;
}

std::unique_ptr<DesktopFrame> GdiCapturer::captureFrame() {
    if (!m_hdcMem || !m_hdcScreen) {
        return nullptr;
    }

    if (!BitBlt(m_hdcMem, 0, 0, m_width, m_height, m_hdcScreen, 0, 0, SRCCOPY)) {
        logging::global().log(logging::Logger::Level::Warning, "GDI: BitBlt falló");
        return nullptr;
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = m_width;
    bmi.bmiHeader.biHeight = -m_height; // Negativo para top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    if (GetDIBits(m_hdcMem, m_hBitmap, 0, m_height, m_buffer.data(), &bmi, DIB_RGB_COLORS) == 0) {
        logging::global().log(logging::Logger::Level::Warning, "GDI: GetDIBits falló");
        return nullptr;
    }

    auto frame = std::make_unique<DesktopFrame>();
    frame->width = m_width;
    frame->height = m_height;
    frame->bgraData = m_buffer;
    frame->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    return frame;
}

} // namespace vic::capture
