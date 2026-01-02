#pragma once

#include "EncodedFrame.h"
#include "InputEvents.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace vic::transport {

struct IceServer {
    std::string url;
    std::optional<std::string> username;
    std::optional<std::string> credential;
    std::optional<std::string> relayTransport; // "udp", "tcp" or "tls"
};

struct SessionDescription {
    std::string type; // "offer" or "answer"
    std::string sdp;
};

struct IceCandidate {
    std::string candidate;
    std::string sdpMid;
    int sdpMLineIndex{0};
};

struct ConnectionInfo {
    std::string code;
    SessionDescription offer;
    std::vector<IceCandidate> iceCandidates;
    std::vector<IceServer> iceServers;
};

enum class ConnectionState {
    New,
    Connecting,
    Connected,
    Disconnected,
    Failed,
    Closed
};

struct TunnelConfig {
    std::string relayHost;
    uint16_t controlPort{9400};
    uint16_t dataPort{9401};
    uint16_t localPort{62020};
};

struct TransportConfig {
    std::vector<IceServer> iceServers;
    uint32_t clockRate{90'000};
    uint32_t ssrc{0x9ec3a4u};
    std::optional<TunnelConfig> tunnel;
};

struct OfferBundle {
    SessionDescription description;
    std::vector<IceCandidate> iceCandidates;
};

struct AnswerBundle {
    SessionDescription description;
    std::vector<IceCandidate> iceCandidates;
};

class TransportServer {
public:
    TransportServer();
    ~TransportServer();

    bool start(const TransportConfig& config);
    void stop();

    OfferBundle createOfferBundle();
    bool applyAnswer(const SessionDescription& answer);
    bool addRemoteCandidate(const IceCandidate& candidate);

    void setConnectionInfo(const ConnectionInfo& info);

    bool sendFrame(const vic::encoder::EncodedFrame& frame);
    
    // Devuelve true una sola vez cuando el DataChannel se abre y necesita keyframe
    bool needsInitialKeyframe();

    void setInputHandlers(
        std::function<void(const vic::input::MouseEvent&)> mouseHandler,
        std::function<void(const vic::input::KeyboardEvent&)> keyboardHandler);

    void setConnectionStateCallback(std::function<void(ConnectionState)> callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class TransportClient {
public:
    TransportClient();
    ~TransportClient();

    bool start(const TransportConfig& config);
    AnswerBundle acceptOffer(const SessionDescription& offer);
    bool addRemoteCandidate(const IceCandidate& candidate);

    void setConnectionInfo(const ConnectionInfo& info);

    void setFrameHandler(std::function<void(const vic::encoder::EncodedFrame&)> handler);
    void setConnectionStateCallback(std::function<void(ConnectionState)> callback);

    bool sendMouseEvent(const vic::input::MouseEvent& ev);
    bool sendKeyboardEvent(const vic::input::KeyboardEvent& ev);

    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vic::transport
