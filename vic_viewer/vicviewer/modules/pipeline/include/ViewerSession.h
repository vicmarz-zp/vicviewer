#pragma once

#include "Transport.h"
#include "VideoDecoder.h"
#include "MatchmakerClient.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace vic::pipeline {

class ViewerSession {
public:
    ViewerSession();
    ~ViewerSession();

    bool connect(const std::string& code);
    
    // Conexión directa por IP:puerto (modo LAN sin internet)
    bool connectDirect(const std::string& hostIp, uint16_t port = 9999);
    
    void disconnect();

    void setFrameCallback(std::function<void(const vic::capture::DesktopFrame&)> callback);

    bool sendMouseEvent(const vic::input::MouseEvent& ev);
    bool sendKeyboardEvent(const vic::input::KeyboardEvent& ev);

    [[nodiscard]] bool isConnected() const { return connected_.load(); }
    void enableAutoReconnect(const std::string& code);
    void disableAutoReconnect();

private:
    std::unique_ptr<vic::transport::TransportClient> client_;
    std::unique_ptr<vic::decoder::VideoDecoder> decoder_;

    std::function<void(const vic::capture::DesktopFrame&)> frameCallback_;
    std::atomic_bool connected_{false};
    std::atomic_bool reconnectRunning_{false};
    std::thread reconnectThread_;
    std::string lastCode_;
    vic::transport::TransportConfig clientTransportConfig_{};
    std::chrono::milliseconds resolveInterval_{3000};
    std::unique_ptr<vic::matchmaking::MatchmakerClient> matchmakerClient_;
};

} // namespace vic::pipeline
