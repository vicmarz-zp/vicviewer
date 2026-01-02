#pragma once

#include "InputEvents.h"

#include <Windows.h>
#include <memory>

namespace vic::input {

class InputInjector {
public:
    InputInjector();
    ~InputInjector();

    // New preferred interface
    bool injectMouse(const MouseEvent& event);
    bool injectKeyboard(const KeyboardEvent& event);
    bool injectEvent(const InputEvent& event);
    
    // Enable/disable input injection
    void setEnabled(bool enabled);
    bool isEnabled() const;
    
    // Utility methods
    static bool getCursorPosition(int32_t& x, int32_t& y);
    static bool getScreenDimensions(int32_t& width, int32_t& height);

    // Legacy interface for compatibility
    void inject(const MouseEvent& mouseEvent);
    void inject(const KeyboardEvent& keyboardEvent);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    
    // Helper methods
    DWORD getMouseButtonFlags(MouseButton button, MouseAction action);
    bool sendInputEvent(const INPUT& input);
};

} // namespace vic::input
