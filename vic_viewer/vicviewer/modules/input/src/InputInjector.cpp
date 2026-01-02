#include "InputInjector.h"
#include "Logger.h"

#include <Windows.h>
#include <algorithm>
#include <mutex>

namespace vic::input {

struct InputInjector::Impl {
    std::mutex mutex_;
    bool enabled_ = true;
    POINT lastCursorPos_ = {};
};

InputInjector::InputInjector() : impl_(std::make_unique<Impl>()) {
    GetCursorPos(&impl_->lastCursorPos_);
}

InputInjector::~InputInjector() = default;

bool InputInjector::injectMouse(const MouseEvent& event) {
    if (!impl_->enabled_) {
        return false;
    }

    std::lock_guard lock(impl_->mutex_);

    int32_t screenWidth, screenHeight;
    if (!getScreenDimensions(screenWidth, screenHeight)) {
        return false;
    }

    // Para movimiento, usar SetCursorPos (mas confiable en Secure Desktop)
    if (event.action == MouseAction::Move || event.absolute) {
        int targetX = event.absolute ? event.x : (impl_->lastCursorPos_.x + event.x);
        int targetY = event.absolute ? event.y : (impl_->lastCursorPos_.y + event.y);
        
        // Clamp to screen bounds
        targetX = (std::max)(0, (std::min)(targetX, screenWidth - 1));
        targetY = (std::max)(0, (std::min)(targetY, screenHeight - 1));
        
        SetCursorPos(targetX, targetY);
        impl_->lastCursorPos_.x = targetX;
        impl_->lastCursorPos_.y = targetY;
        
        if (event.action == MouseAction::Move) {
            return true;
        }
    }

    // Para clics, usar mouse_event (funciona mejor en Secure Desktop)
    DWORD flags = 0;
    DWORD mouseData = 0;
    
    switch (event.action) {
    case MouseAction::Down:
    case MouseAction::Up:
        flags = getMouseButtonFlags(event.button, event.action);
        if (event.button == MouseButton::X1) {
            mouseData = XBUTTON1;
        } else if (event.button == MouseButton::X2) {
            mouseData = XBUTTON2;
        }
        break;
        
    case MouseAction::Wheel:
        flags = MOUSEEVENTF_WHEEL;
        mouseData = static_cast<DWORD>(event.wheelDelta);
        break;
        
    default:
        return true;
    }

    // Usar mouse_event en lugar de SendInput para mejor compatibilidad
    mouse_event(flags, 0, 0, mouseData, 0);
    return true;
}

bool InputInjector::injectKeyboard(const KeyboardEvent& event) {
    if (!impl_->enabled_) {
        return false;
    }

    std::lock_guard lock(impl_->mutex_);

    // Usar keybd_event en lugar de SendInput para mejor compatibilidad en Secure Desktop
    DWORD flags = 0;
    
    if (event.action == KeyAction::Up) {
        flags |= KEYEVENTF_KEYUP;
    }
    
    if (event.extended) {
        flags |= KEYEVENTF_EXTENDEDKEY;
    }

    // keybd_event(vk, scan, flags, extraInfo)
    keybd_event(
        static_cast<BYTE>(event.virtualKey),
        static_cast<BYTE>(event.scanCode),
        flags,
        0
    );
    
    return true;
}

bool InputInjector::injectEvent(const InputEvent& event) {
    switch (event.type) {
    case InputEvent::Mouse:
        return injectMouse(event.mouse);
    case InputEvent::Keyboard:
        return injectKeyboard(event.keyboard);
    default:
        return false;
    }
}

void InputInjector::setEnabled(bool enabled) {
    impl_->enabled_ = enabled;
    if (enabled) {
        logging::global().log(logging::Logger::Level::Info, "Input injection enabled");
    } else {
        logging::global().log(logging::Logger::Level::Info, "Input injection disabled");
    }
}

bool InputInjector::isEnabled() const {
    return impl_->enabled_;
}

bool InputInjector::getCursorPosition(int32_t& x, int32_t& y) {
    POINT point;
    if (GetCursorPos(&point)) {
        x = point.x;
        y = point.y;
        return true;
    }
    return false;
}

bool InputInjector::getScreenDimensions(int32_t& width, int32_t& height) {
    width = GetSystemMetrics(SM_CXSCREEN);
    height = GetSystemMetrics(SM_CYSCREEN);
    return width > 0 && height > 0;
}

// Legacy methods for compatibility
void InputInjector::inject(const MouseEvent& mouseEvent) {
    injectMouse(mouseEvent);
}

void InputInjector::inject(const KeyboardEvent& keyboardEvent) {
    injectKeyboard(keyboardEvent);
}

DWORD InputInjector::getMouseButtonFlags(MouseButton button, MouseAction action) {
    DWORD flags = 0;
    
    bool isDown = (action == MouseAction::Down);
    
    switch (button) {
    case MouseButton::Left:
        flags = isDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        break;
    case MouseButton::Right:
        flags = isDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        break;
    case MouseButton::Middle:
        flags = isDown ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        break;
    case MouseButton::X1:
        flags = isDown ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        // Note: mouseData will be set separately for X buttons
        break;
    case MouseButton::X2:
        flags = isDown ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        // Note: mouseData will be set separately for X buttons
        break;
    }
    
    return flags;
}

bool InputInjector::sendInputEvent(const INPUT& input) {
    UINT result = SendInput(1, const_cast<INPUT*>(&input), sizeof(INPUT));
    
    if (result != 1) {
        DWORD error = GetLastError();
        logging::global().log(logging::Logger::Level::Error, 
            "SendInput failed with error: " + std::to_string(error));
        return false;
    }
    
    return true;
}

} // namespace vic::input
