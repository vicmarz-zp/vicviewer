#pragma once

#include <cstdint>
#include <vector>

namespace vic::encoder {

struct EncodedFrame {
    uint64_t timestamp{};
    std::vector<uint8_t> payload{};
    uint32_t width{};           // Ancho del frame codificado (puede estar escalado)
    uint32_t height{};          // Alto del frame codificado
    uint32_t originalWidth{};   // Ancho ORIGINAL de la pantalla (para coordenadas de mouse)
    uint32_t originalHeight{};  // Alto ORIGINAL de la pantalla
    bool keyFrame{false};
};

} // namespace vic::encoder
