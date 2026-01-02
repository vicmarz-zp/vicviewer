#pragma once

#include "DesktopFrame.h"

#include <Windows.h>
#include <cstdint>
#include <memory>

namespace vic::ui {

/// Interface para renderizadores de frames
class FrameRenderer {
public:
    virtual ~FrameRenderer() = default;

    /// Inicializar el renderer para una ventana específica
    virtual bool Initialize(HWND hwnd) = 0;
    
    /// Redimensionar cuando cambia el tamaño de la ventana
    virtual void Resize(uint32_t width, uint32_t height) = 0;
    
    /// Renderizar un frame
    virtual void RenderFrame(const vic::capture::DesktopFrame& frame) = 0;
    
    /// Presentar (llamar después de RenderFrame)
    virtual void Present() = 0;
    
    /// Limpiar recursos
    virtual void Shutdown() = 0;
    
    /// Obtener nombre del renderer para diagnóstico
    virtual const char* GetName() const = 0;
    
    /// Verificar si está inicializado correctamente
    virtual bool IsValid() const = 0;
};

/// Crear el mejor renderer disponible:
/// 1. Direct3D 11 (si GPU compatible)
/// 2. GDI fallback (siempre disponible)
std::unique_ptr<FrameRenderer> CreateBestRenderer();

/// Crear renderer Direct3D 11 específicamente
std::unique_ptr<FrameRenderer> CreateD3D11Renderer();

/// Crear renderer GDI (fallback)
std::unique_ptr<FrameRenderer> CreateGdiRenderer();

} // namespace vic::ui
