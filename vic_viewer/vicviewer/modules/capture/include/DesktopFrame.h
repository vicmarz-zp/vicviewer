#pragma once

#include <cstdint>
#include <vector>

namespace vic::capture {

struct DesktopFrame {
    uint32_t width{};
    uint32_t height{};
    uint32_t originalWidth{};     // Resolución original antes del escalado (para coordenadas de mouse)
    uint32_t originalHeight{};    // Resolución original antes del escalado (para coordenadas de mouse)
    uint64_t timestamp{};
    std::vector<uint8_t> bgraData{};
};

} // namespace vic::capture
