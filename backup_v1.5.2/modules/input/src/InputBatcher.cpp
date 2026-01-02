#include "InputBatcher.h"
#include "Logger.h"

namespace vic::input {

InputBatcher::InputBatcher() = default;
InputBatcher::~InputBatcher() {
    // Log stats on destruction
    if (totalMouseEvents_ > 0) {
        double savings = (coalescedMouseEvents_ * 100.0) / totalMouseEvents_;
        logging::global().log(logging::Logger::Level::Info,
            "[InputBatcher] Total mouse events: " + std::to_string(totalMouseEvents_) +
            ", Coalesced: " + std::to_string(coalescedMouseEvents_) +
            " (saved " + std::to_string(static_cast<int>(savings)) + "%)");
    }
}

bool InputBatcher::addMouseEvent(const MouseEvent& event) {
    std::lock_guard lock(mutex_);
    totalMouseEvents_++;
    
    // Clicks and wheel events are immediate - don't batch
    if (event.action == MouseAction::Down || 
        event.action == MouseAction::Up ||
        event.action == MouseAction::Wheel) {
        // If we have a pending move, add it first
        if (lastMouseMove_) {
            pendingMouse_.push_back(*lastMouseMove_);
            lastMouseMove_.reset();
        }
        pendingMouse_.push_back(event);
        return true;  // Send batch immediately
    }
    
    // Mouse move - coalesce with previous move
    if (event.action == MouseAction::Move) {
        if (coalesceMouseMoves_ && lastMouseMove_) {
            coalescedMouseEvents_++;
            // Replace previous move with this one (coalesce)
            lastMouseMove_ = event;
            return false;  // Don't send yet
        }
        lastMouseMove_ = event;
        return false;
    }
    
    pendingMouse_.push_back(event);
    return false;
}

bool InputBatcher::addKeyboardEvent(const KeyboardEvent& event) {
    std::lock_guard lock(mutex_);
    
    // Keyboard events are always queued but trigger immediate send
    // to minimize input latency
    pendingKeyboard_.push_back(event);
    return true;  // Always send keyboard immediately
}

InputBatcher::BatchedEvents InputBatcher::flush() {
    std::lock_guard lock(mutex_);
    
    BatchedEvents result;
    
    // Add pending mouse move if any
    if (lastMouseMove_) {
        pendingMouse_.push_back(*lastMouseMove_);
        lastMouseMove_.reset();
    }
    
    // Move all pending events to result
    result.mouseEvents.reserve(pendingMouse_.size());
    for (auto& ev : pendingMouse_) {
        result.mouseEvents.push_back(std::move(ev));
    }
    pendingMouse_.clear();
    
    result.keyboardEvents.reserve(pendingKeyboard_.size());
    for (auto& ev : pendingKeyboard_) {
        result.keyboardEvents.push_back(std::move(ev));
    }
    pendingKeyboard_.clear();
    
    result.hasImmediateEvents = !result.mouseEvents.empty() || !result.keyboardEvents.empty();
    
    return result;
}

bool InputBatcher::hasPendingEvents() const {
    std::lock_guard lock(mutex_);
    return !pendingMouse_.empty() || !pendingKeyboard_.empty() || lastMouseMove_.has_value();
}

} // namespace vic::input
