#pragma once

#include "InputInjector.h"

#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace vic::input {

/// Batches and coalesces input events to reduce network overhead
/// - Mouse move events are coalesced (only latest position sent)
/// - Key events are batched but not coalesced (order matters)
/// - Clicks are sent immediately (low latency requirement)
class InputBatcher {
public:
    struct BatchedEvents {
        std::vector<MouseEvent> mouseEvents;
        std::vector<KeyboardEvent> keyboardEvents;
        bool hasImmediateEvents = false;  // Clicks, key presses
    };

    InputBatcher();
    ~InputBatcher();

    /// Add a mouse event to the batch
    /// Returns true if this event should be sent immediately (click)
    bool addMouseEvent(const MouseEvent& event);

    /// Add a keyboard event to the batch
    /// Returns true if this event should be sent immediately
    bool addKeyboardEvent(const KeyboardEvent& event);

    /// Get all pending events and clear the batch
    /// Called periodically (e.g., every 5-16ms)
    BatchedEvents flush();

    /// Check if there are pending events
    bool hasPendingEvents() const;

    /// Configure coalescing behavior
    void setCoalesceMouseMoves(bool enable) { coalesceMouseMoves_ = enable; }
    void setBatchInterval(std::chrono::milliseconds interval) { batchInterval_ = interval; }

private:
    mutable std::mutex mutex_;
    
    // Pending events
    std::deque<MouseEvent> pendingMouse_;
    std::deque<KeyboardEvent> pendingKeyboard_;
    
    // Last mouse position (for coalescing moves)
    std::optional<MouseEvent> lastMouseMove_;
    
    // Configuration
    bool coalesceMouseMoves_ = true;
    std::chrono::milliseconds batchInterval_{5};
    
    // Stats
    uint64_t totalMouseEvents_ = 0;
    uint64_t coalescedMouseEvents_ = 0;
};

} // namespace vic::input
