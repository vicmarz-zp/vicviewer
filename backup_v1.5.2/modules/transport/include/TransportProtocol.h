#pragma once

#include <cstdint>

namespace vic::transport::protocol {

enum class ControlMessageType : uint8_t {
    Mouse = 1,
    Keyboard = 2,
    VideoFrame = 3
};

#pragma pack(push, 1)
struct VideoFrameHeader {
    uint32_t width;           // Ancho del frame (puede estar escalado)
    uint32_t height;          // Alto del frame (puede estar escalado)
    uint64_t timestamp;
    uint32_t payloadSize;
    uint8_t keyFrame;
    uint32_t originalWidth;   // Ancho ORIGINAL de la pantalla del host
    uint32_t originalHeight;  // Alto ORIGINAL de la pantalla del host
};
#pragma pack(pop)

#pragma pack(push, 1)
struct MouseMessage {
    int32_t x;
    int32_t y;
    int32_t wheel;
    uint8_t action;
    uint8_t button;
};

struct KeyboardMessage {
    uint16_t vk;
    uint16_t scan;
    uint8_t action;
};
#pragma pack(pop)

} // namespace vic::transport::protocol
