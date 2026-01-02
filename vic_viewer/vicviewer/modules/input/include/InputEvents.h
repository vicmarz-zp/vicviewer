#pragma once

#include <Windows.h>
#include <cstdint>

namespace vic::input {

enum class MouseButton : uint8_t {
    Left,
    Right,
    Middle,
    X1,
    X2
};

enum class KeyAction : uint8_t {
    Down,
    Up
};

enum class MouseAction : uint8_t {
    Down,
    Up,
    Move,
    Wheel
};

struct MouseEvent {
    MouseAction action{};
    MouseButton button{};
    int32_t x{};
    int32_t y{};
    int32_t wheelDelta{};
    bool absolute{true};  // true = absolute coordinates, false = relative
};

struct KeyboardEvent {
    KeyAction action{};
    uint16_t virtualKey{};
    uint16_t scanCode{};
    bool extended{false};  // true for extended keys (arrows, etc.)
    bool alt{false};
    bool ctrl{false};
    bool shift{false};
};

struct InputEvent {
    enum Type : uint8_t {
        Mouse = 0,
        Keyboard = 1
    } type;

    uint32_t timestamp{};

    union {
        MouseEvent mouse;
        KeyboardEvent keyboard;
    };

    InputEvent() : type(Mouse), timestamp(0) {
        mouse = {};
    }
};

} // namespace vic::input
