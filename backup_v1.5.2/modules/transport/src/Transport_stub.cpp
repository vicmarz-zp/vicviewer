// Temporary stub implementation without WebRTC
#include "Transport.h"
#include "Logger.h"
#include "InputEvents.h"
#include <memory>

namespace vic::transport {

struct WebRtcServer::Impl {
    bool started = false;
};

struct WebRtcClient::Impl {
    bool connected = false;
};

WebRtcServer::WebRtcServer(const TransportConfig& config) 
    : impl_(std::make_unique<Impl>()) {
}

WebRtcServer::~WebRtcServer() = default;

bool WebRtcServer::start(std::function<void(const OfferBundle&)> onOffer,
                        std::function<void(const IceCandidate&)> onCandidate) {
    logging::global().log(logging::Logger::Level::Info, "WebRTC server stub - start");
    impl_->started = true;
    return true;
}

bool WebRtcServer::applyAnswer(const AnswerBundle& bundle) {
    logging::global().log(logging::Logger::Level::Info, "WebRTC server stub - apply answer");
    return true;
}

void WebRtcServer::addCandidate(const IceCandidate& candidate) {
    logging::global().log(logging::Logger::Level::Info, "WebRTC server stub - add candidate");
}

void WebRtcServer::sendFrame(const std::vector<uint8_t>& frame, uint32_t timestamp) {
    // Stub - no actual transmission
}

void WebRtcServer::setConnectionCallback(std::function<void(ConnectionState)> callback) {
    if (callback) {
        callback(ConnectionState::Connected);
    }
}

void WebRtcServer::setInputCallback(std::function<void(const vic::input::InputEvent&)> callback) {
    // Stub - no input handling
}

WebRtcClient::WebRtcClient(const TransportConfig& config)
    : impl_(std::make_unique<Impl>()) {
}

WebRtcClient::~WebRtcClient() = default;

bool WebRtcClient::start(const ConnectionInfo& info,
                        std::function<void(const AnswerBundle&)> onAnswer,
                        std::function<void(const IceCandidate&)> onCandidate) {
    logging::global().log(logging::Logger::Level::Info, "WebRTC client stub - start");
    impl_->connected = true;
    
    // Generate dummy answer
    if (onAnswer) {
        AnswerBundle answer;
        answer.description.type = "answer";
        answer.description.sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n";
        onAnswer(answer);
    }
    
    return true;
}

void WebRtcClient::setFrameCallback(std::function<void(const std::vector<uint8_t>&, uint32_t)> callback) {
    // Stub - no frame reception
}

void WebRtcClient::setConnectionCallback(std::function<void(ConnectionState)> callback) {
    if (callback) {
        callback(ConnectionState::Connected);
    }
}

void WebRtcClient::sendInput(const vic::input::InputEvent& event) {
    // Stub - no input transmission
}

} // namespace vic::transport