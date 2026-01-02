#pragma once

#include "DesktopCapturer.h"
#include "FrameScaler.h"
#include "InputInjector.h"
#include "Transport.h"
#include "VideoEncoder.h"
#include "MatchmakerClient.h"
#include "StreamConfig.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <chrono>

namespace vic::pipeline {

class HostSession {
public:
    HostSession();
    ~HostSession();

    bool start(uint16_t port = 50050);
    void stop();
    
    /// Establecer un código fijo (debe llamarse antes de start())
    void setFixedCode(const std::string& code) { fixedCode_ = code; }
    
    /// Indicar que el registro en matchmaker se maneja externamente
    void setExternalRegistration(bool external) { externalRegistration_ = external; if (external) registered_.store(true); }
    
    /// Establecer URL del matchmaker (debe llamarse antes de start())
    void setMatchmakerUrl(const std::wstring& url) { matchmakerUrl_ = url; }
    
    /// Pasar un MatchmakerClient existente (debe llamarse antes de start())
    void setMatchmakerClient(std::unique_ptr<vic::matchmaking::MatchmakerClient> client) { 
        matchmakerClient_ = std::move(client); 
    }
    
    /// Configurar calidad del stream (debe llamarse antes de start())
    void setStreamConfig(const StreamConfig& config) { streamConfig_ = config; }
    StreamConfig& streamConfig() { return streamConfig_; }

    [[nodiscard]] std::optional<vic::transport::ConnectionInfo> connectionInfo() const { return connectionInfo_; }
    [[nodiscard]] bool isRunning() const { return running_.load(); }
    [[nodiscard]] bool hasCapturedFrame() const { return lastFrameTimestampMs_.load() != 0; }
    [[nodiscard]] bool isViewerConnected() const { return answerApplied_.load(); }
    
    // Métricas
    [[nodiscard]] uint32_t currentFps() const { return currentFps_.load(); }
    [[nodiscard]] uint32_t currentBitrate() const { return currentBitrateKbps_.load(); }

private:
    void captureLoop();
    void signalingLoop();

    std::unique_ptr<vic::capture::DesktopCapturer> capturer_;
    std::unique_ptr<vic::capture::FrameScaler> scaler_;
    std::unique_ptr<vic::encoder::VideoEncoder> encoder_;
    std::unique_ptr<vic::input::InputInjector> inputInjector_;
    std::unique_ptr<vic::transport::TransportServer> transportServer_;

    std::atomic_bool running_{false};
    std::thread captureThread_;
    std::thread signalingThread_;
    std::optional<vic::transport::ConnectionInfo> connectionInfo_;
    std::atomic_bool registered_{false};
    bool externalRegistration_{false};  // Si true, no hacer registro interno
    std::wstring matchmakerUrl_ = vic::matchmaking::MatchmakerClient::kDefaultServiceUrl;
    std::unique_ptr<vic::matchmaking::MatchmakerClient> matchmakerClient_;
    std::chrono::milliseconds retryInterval_{3000};
    std::atomic<uint64_t> lastFrameTimestampMs_{0};
    std::atomic_bool answerApplied_{false};
    vic::transport::TransportConfig transportConfig_{};
    std::string fixedCode_;  // Código fijo opcional
    
    // Configuración de stream
    StreamConfig streamConfig_;
    
    // Métricas en tiempo real
    std::atomic<uint32_t> currentFps_{0};
    std::atomic<uint32_t> currentBitrateKbps_{0};
    std::atomic<uint64_t> frameCount_{0};
    std::atomic<uint64_t> bytesSent_{0};
    
    // Servidor TCP para conexiones LAN directas
    std::thread lanServerThread_;
    std::atomic_bool lanServerRunning_{false};
    static constexpr uint16_t LAN_PORT = 9999;
    void lanServerLoop();
};

} // namespace vic::pipeline
