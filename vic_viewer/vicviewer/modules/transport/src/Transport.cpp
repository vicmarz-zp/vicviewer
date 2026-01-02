#include "Transport.h"

#include "Logger.h"
#include "TransportProtocol.h"
#include "TunnelAgent.h"
#include "TunnelFallback.h"

#include <rtc/rtc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace vic::transport {

namespace {

using protocol::ControlMessageType;
using protocol::KeyboardMessage;
using protocol::MouseMessage;

std::atomic_bool g_rtcInitialized{false};

void ensureRtcInitialized() {
    if (!g_rtcInitialized.load(std::memory_order_acquire)) {
        rtc::InitLogger(rtc::LogLevel::Warning);
        g_rtcInitialized.store(true, std::memory_order_release);
    }
}

rtc::Configuration buildRtcConfiguration(const TransportConfig& config) {
    rtc::Configuration rtcConfig;
    for (const auto& server : config.iceServers) {
        rtc::IceServer rtcServer(server.url);
        if (server.username) {
            rtcServer.username = *server.username;
        }
        if (server.credential) {
            rtcServer.password = *server.credential;
        }
        if (server.relayTransport) {
            const std::string& transport = *server.relayTransport;
            if (transport == "udp") {
                rtcServer.relayType = rtc::IceServer::RelayType::TurnUdp;
            } else if (transport == "tcp") {
                rtcServer.relayType = rtc::IceServer::RelayType::TurnTcp;
            } else if (transport == "tls") {
                rtcServer.relayType = rtc::IceServer::RelayType::TurnTls;
            }
        }
        rtcConfig.iceServers.emplace_back(std::move(rtcServer));
    }
    // Usar negociación manual para control completo del flujo offer/answer
    rtcConfig.disableAutoNegotiation = true;
    return rtcConfig;
}

uint32_t normalizeTimestamp(uint64_t timestampMs, uint32_t clockRate) {
    const double seconds = static_cast<double>(timestampMs) / 1000.0;
    const double ticks = seconds * static_cast<double>(clockRate);
    const uint64_t wrapped = static_cast<uint64_t>(ticks) & 0xFFFFFFFFu;
    return static_cast<uint32_t>(wrapped);
}

uint64_t extractTimestampMs(const rtc::FrameInfo& info, uint32_t clockRate) {
    if (info.timestampSeconds) {
        return static_cast<uint64_t>(info.timestampSeconds->count() * 1000.0);
    }
    const double seconds = static_cast<double>(info.timestamp) / static_cast<double>(clockRate);
    return static_cast<uint64_t>(seconds * 1000.0);
}

struct Vp8Metadata {
    bool keyFrame{false};
    uint32_t width{0};
    uint32_t height{0};
};

Vp8Metadata parseVp8Metadata(const rtc::binary& data, uint32_t currentWidth, uint32_t currentHeight) {
    Vp8Metadata meta{};
    meta.width = currentWidth;
    meta.height = currentHeight;

    if (data.size() < 10) {
        return meta;
    }

    const uint8_t* raw = reinterpret_cast<const uint8_t*>(data.data());
    const bool key = (raw[0] & 0x01u) == 0;
    meta.keyFrame = key;

    if (!key) {
        return meta;
    }

    if (raw[3] == 0x9d && raw[4] == 0x01 && raw[5] == 0x2a) {
        const uint16_t rawWidth = static_cast<uint16_t>(raw[6]) | (static_cast<uint16_t>(raw[7]) << 8);
        const uint16_t rawHeight = static_cast<uint16_t>(raw[8]) | (static_cast<uint16_t>(raw[9]) << 8);
        meta.width = rawWidth & 0x3FFF;
        meta.height = rawHeight & 0x3FFF;
    }
    return meta;
}

rtc::binary buildMousePayload(const vic::input::MouseEvent& ev) {
    rtc::binary payload;
    payload.resize(sizeof(uint8_t) + sizeof(MouseMessage));
    payload[0] = std::byte{static_cast<uint8_t>(ControlMessageType::Mouse)};
    MouseMessage message{};
    message.x = ev.x;
    message.y = ev.y;
    message.wheel = ev.wheelDelta;
    message.action = static_cast<uint8_t>(ev.action);
    message.button = static_cast<uint8_t>(ev.button);
    std::memcpy(payload.data() + 1, &message, sizeof(MouseMessage));
    return payload;
}

rtc::binary buildKeyboardPayload(const vic::input::KeyboardEvent& ev) {
    rtc::binary payload;
    payload.resize(sizeof(uint8_t) + sizeof(KeyboardMessage));
    payload[0] = std::byte{static_cast<uint8_t>(ControlMessageType::Keyboard)};
    KeyboardMessage message{};
    message.vk = ev.virtualKey;
    message.scan = ev.scanCode;
    message.action = static_cast<uint8_t>(ev.action);
    std::memcpy(payload.data() + 1, &message, sizeof(KeyboardMessage));
    return payload;
}

ConnectionState mapState(rtc::PeerConnection::State state) {
    switch (state) {
    case rtc::PeerConnection::State::New:
        return ConnectionState::New;
    case rtc::PeerConnection::State::Connecting:
        return ConnectionState::Connecting;
    case rtc::PeerConnection::State::Connected:
        return ConnectionState::Connected;
    case rtc::PeerConnection::State::Disconnected:
        return ConnectionState::Disconnected;
    case rtc::PeerConnection::State::Failed:
        return ConnectionState::Failed;
    case rtc::PeerConnection::State::Closed:
        return ConnectionState::Closed;
    default:
        return ConnectionState::Failed;
    }
}

SessionDescription toSessionDescription(const rtc::Description& description) {
    SessionDescription result;
    result.type = description.typeString();
    result.sdp = description.generateSdp();
    return result;
}

IceCandidate toIceCandidate(const rtc::Candidate& candidate) {
    IceCandidate cnd;
    cnd.candidate = candidate.candidate();
    cnd.sdpMid = candidate.mid();
    cnd.sdpMLineIndex = 0;
    return cnd;
}

rtc::Candidate fromIceCandidate(const IceCandidate& candidate) {
    if (!candidate.sdpMid.empty()) {
        return rtc::Candidate(candidate.candidate, candidate.sdpMid);
    }
    return rtc::Candidate(candidate.candidate);
}

struct LocalGatheringState {
    std::mutex mutex;
    std::condition_variable descriptionCv;
    std::condition_variable gatheringCv;
    bool descriptionReady{false};
    bool gatheringComplete{false};
    SessionDescription localDescription;
    std::vector<IceCandidate> localCandidates;
};

class DataChannelWrapper {
public:
    void attach(const std::shared_ptr<rtc::DataChannel>& channel,
                std::function<void(const vic::input::MouseEvent&)> mouseHandler,
                std::function<void(const vic::input::KeyboardEvent&)> keyboardHandler,
                std::function<void(const vic::encoder::EncodedFrame&)> frameHandler = nullptr) {
        channel_ = channel;
        mouseHandler_ = std::move(mouseHandler);
        keyboardHandler_ = std::move(keyboardHandler);
        frameHandler_ = std::move(frameHandler);

        if (!channel_) {
            logging::global().log(logging::Logger::Level::Warning, "[DC] attach: channel es NULL");
            return;
        }

        logging::global().log(logging::Logger::Level::Info, 
            std::string("[DC] attach: frameHandler=") + (frameHandler_ ? "SET" : "NULL"));

        channel_->onMessage([
            this
        ](rtc::message_variant message) {
            std::visit([this](auto&& arg) { handleMessage(arg); }, message);
        });
    }

    bool send(const rtc::binary& payload) {
        if (!channel_) {
            logging::global().log(logging::Logger::Level::Warning, "[DC] send: channel is null");
            return false;
        }
        if (!channel_->isOpen()) {
            logging::global().log(logging::Logger::Level::Warning, "[DC] send: channel not open");
            return false;
        }
        logging::global().log(logging::Logger::Level::Debug, "[DC] send: enviando " + std::to_string(payload.size()) + " bytes");
        return channel_->send(payload);
    }

private:
    void handleMessage(const rtc::binary& data) {
        if (data.size() <= 1) {
            return;
        }
        const uint8_t type = std::to_integer<uint8_t>(data[0]);
        const std::byte* buffer = data.data() + 1;
        
        logging::global().log(logging::Logger::Level::Debug, 
            "[DC] handleMessage: type=" + std::to_string(type) + " size=" + std::to_string(data.size()));
        
    if (type == static_cast<uint8_t>(ControlMessageType::Mouse)) {
            if (data.size() != 1 + sizeof(MouseMessage)) {
                return;
            }
            MouseMessage msg{};
            std::memcpy(&msg, buffer, sizeof(MouseMessage));
            if (mouseHandler_) {
                vic::input::MouseEvent evt{};
                evt.x = msg.x;
                evt.y = msg.y;
                evt.wheelDelta = msg.wheel;
                evt.action = static_cast<vic::input::MouseAction>(msg.action);
                evt.button = static_cast<vic::input::MouseButton>(msg.button);
                mouseHandler_(evt);
            }
    } else if (type == static_cast<uint8_t>(ControlMessageType::Keyboard)) {
            if (data.size() != 1 + sizeof(KeyboardMessage)) {
                return;
            }
            KeyboardMessage msg{};
            std::memcpy(&msg, buffer, sizeof(KeyboardMessage));
            if (keyboardHandler_) {
                vic::input::KeyboardEvent evt{};
                evt.virtualKey = msg.vk;
                evt.scanCode = msg.scan;
                evt.action = static_cast<vic::input::KeyAction>(msg.action);
                keyboardHandler_(evt);
            }
        } else if (type == static_cast<uint8_t>(ControlMessageType::VideoFrame)) {
            const size_t headerSize = sizeof(protocol::VideoFrameHeader);
            if (data.size() < 1 + headerSize) {
                logging::global().log(logging::Logger::Level::Warning, "[DC] VideoFrame: header too small");
                return;
            }
            protocol::VideoFrameHeader header{};
            std::memcpy(&header, buffer, headerSize);
            
            if (data.size() != 1 + headerSize + header.payloadSize) {
                logging::global().log(logging::Logger::Level::Warning, "[DC] VideoFrame: size mismatch");
                return;
            }
            
            logging::global().log(logging::Logger::Level::Info, 
                "[DC] VideoFrame RECIBIDO: " + std::to_string(header.width) + "x" + 
                std::to_string(header.height) + " (orig:" + std::to_string(header.originalWidth) + "x" +
                std::to_string(header.originalHeight) + ") payload=" + std::to_string(header.payloadSize));
            
            if (frameHandler_) {
                vic::encoder::EncodedFrame frame{};
                frame.width = header.width;
                frame.height = header.height;
                frame.originalWidth = header.originalWidth > 0 ? header.originalWidth : header.width;
                frame.originalHeight = header.originalHeight > 0 ? header.originalHeight : header.height;
                frame.timestamp = header.timestamp;
                frame.keyFrame = header.keyFrame != 0;
                frame.payload.resize(header.payloadSize);
                std::memcpy(frame.payload.data(), buffer + headerSize, header.payloadSize);
                logging::global().log(logging::Logger::Level::Info, "[DC] Llamando frameHandler_");
                frameHandler_(frame);
            } else {
                logging::global().log(logging::Logger::Level::Warning, "[DC] frameHandler_ es NULL!");
            }
        }
    }

    void handleMessage(const std::string&) {}

    std::shared_ptr<rtc::DataChannel> channel_;
    std::function<void(const vic::input::MouseEvent&)> mouseHandler_;
    std::function<void(const vic::input::KeyboardEvent&)> keyboardHandler_;
    std::function<void(const vic::encoder::EncodedFrame&)> frameHandler_;
};

} // namespace

class TransportServer::Impl {
public:
    bool needsInitialKeyframe() {
        return needsKeyframe_.exchange(false, std::memory_order_acq_rel);
    }

    bool start(const TransportConfig& config) {
        ensureRtcInitialized();
        stop();

        config_ = config;
        rtc::Configuration rtcConfig = buildRtcConfiguration(config);

        try {
            pc_ = std::make_shared<rtc::PeerConnection>(rtcConfig);
        } catch (const std::exception& ex) {
            logging::global().log(logging::Logger::Level::Error,
                std::string("TransportServer: failed to create PeerConnection: ") + ex.what());
            return false;
        }

        state_.store(ConnectionState::New);
        peerState_.store(ConnectionState::New);
        fallbackConnected_.store(false);
        setupCallbacks();
        setupMedia();
        setupDataChannel();
        ensureFallbackInitialized();
        recomputeState();
        return true;
    }

    void stop() {
        if (pc_) {
            pc_->close();
        }
        pc_.reset();
        videoTrack_.reset();
        controlChannel_ = {};
        teardownFallback();
        {
            std::lock_guard lock(gatheringState_.mutex);
            gatheringState_.descriptionReady = false;
            gatheringState_.gatheringComplete = false;
            gatheringState_.localCandidates.clear();
        }
        state_.store(ConnectionState::Closed);
        peerState_.store(ConnectionState::Closed);
        recomputeState();
    }

    OfferBundle createOfferBundle() {
        if (!pc_) {
            throw std::runtime_error("TransportServer not started");
        }

        {
            std::lock_guard lock(gatheringState_.mutex);
            gatheringState_.descriptionReady = false;
            gatheringState_.gatheringComplete = false;
            gatheringState_.localCandidates.clear();
        }

        // Con disableAutoNegotiation=true, debemos generar explícitamente oferta+ICE
        pc_->setLocalDescription(rtc::Description::Type::Offer);
        
        // Disparar gathering de candidatos ICE manualmente
        try {
            pc_->gatherLocalCandidates();
        } catch (const std::exception& ex) {
            logging::global().log(logging::Logger::Level::Warning,
                std::string("Failed to gather candidates: ") + ex.what());
        }

        std::unique_lock lock(gatheringState_.mutex);
        logging::global().log(logging::Logger::Level::Debug, "TransportServer: waiting for local description");
        gatheringState_.descriptionCv.wait(lock, [this]() {
            return gatheringState_.descriptionReady;
        });
        logging::global().log(logging::Logger::Level::Debug, "TransportServer: local description ready, awaiting ICE completion");

        gatheringState_.gatheringCv.wait_for(lock, std::chrono::seconds(10), [this]() {
            return gatheringState_.gatheringComplete;
        });
        logging::global().log(logging::Logger::Level::Debug, "TransportServer: ICE wait finished");

        OfferBundle bundle;
        bundle.description = gatheringState_.localDescription;
        bundle.iceCandidates = gatheringState_.localCandidates;
        return bundle;
    }

    bool applyAnswer(const SessionDescription& answer) {
        if (!pc_) {
            return false;
        }
        try {
            rtc::Description description(answer.sdp, answer.type);
            pc_->setRemoteDescription(std::move(description));
            return true;
        } catch (const std::exception& ex) {
            logging::global().log(logging::Logger::Level::Error,
                std::string("TransportServer: failed to apply answer: ") + ex.what());
            return false;
        }
    }

    bool addRemoteCandidate(const IceCandidate& candidate) {
        if (!pc_) {
            return false;
        }
        try {
            pc_->addRemoteCandidate(fromIceCandidate(candidate));
            return true;
        } catch (const std::exception& ex) {
            logging::global().log(logging::Logger::Level::Warning,
                std::string("TransportServer: failed to add remote candidate: ") + ex.what());
            return false;
        }
    }

    bool sendFrame(const vic::encoder::EncodedFrame& frame) {
        bool sent = false;
        
        // NOTA: Usar DataChannel para video para mejor compatibilidad WAN
        // El track RTP tiene problemas con sdpMid mismatch entre host y viewer
        
        // Send video via DataChannel (mejor compatibilidad)
        if (controlChannel_ && controlChannel_->isOpen() && !frame.payload.empty()) {
            sent = sendFrameViaDataChannel(frame);
        }
        
        // Fallback: Try RTP track only if DataChannel failed
        if (!sent && videoTrack_ && videoTrack_->isOpen() && !frame.payload.empty()) {
            const uint32_t timestamp = normalizeTimestamp(frame.timestamp, config_.clockRate);
            rtc::FrameInfo info(timestamp);
            info.timestampSeconds = std::chrono::duration<double>(frame.timestamp / 1000.0);
            const auto* data = reinterpret_cast<const std::byte*>(frame.payload.data());
            videoTrack_->sendFrame(data, frame.payload.size(), info);
            sent = true;
        }
        
        if (fallbackServer_) {
            if (fallbackServer_->sendFrame(frame)) {
                sent = true;
            }
        }
        return sent;
    }
    
    bool sendFrameViaDataChannel(const vic::encoder::EncodedFrame& frame) {
        if (!controlChannel_ || !controlChannel_->isOpen()) {
            static int logCount = 0;
            if (logCount++ % 100 == 0) {
                logging::global().log(logging::Logger::Level::Debug, 
                    "[Server] sendFrameViaDataChannel: channel not ready");
            }
            return false;
        }
        
        // Build packet: [type:1][header][payload:N]
        const size_t headerSize = sizeof(protocol::VideoFrameHeader);
        rtc::binary packet;
        packet.resize(1 + headerSize + frame.payload.size());
        
        packet[0] = std::byte{static_cast<uint8_t>(protocol::ControlMessageType::VideoFrame)};
        
        protocol::VideoFrameHeader header{};
        header.width = frame.width;
        header.height = frame.height;
        header.timestamp = frame.timestamp;
        header.payloadSize = static_cast<uint32_t>(frame.payload.size());
        header.keyFrame = frame.keyFrame ? 1 : 0;
        // Resolución original para cálculo correcto de coordenadas de mouse
        header.originalWidth = frame.originalWidth > 0 ? frame.originalWidth : frame.width;
        header.originalHeight = frame.originalHeight > 0 ? frame.originalHeight : frame.height;
        
        std::memcpy(packet.data() + 1, &header, headerSize);
        std::memcpy(packet.data() + 1 + headerSize, frame.payload.data(), frame.payload.size());
        
        static int frameCount = 0;
        if (frameCount++ % 30 == 0) {
            logging::global().log(logging::Logger::Level::Info, 
                "[Server] Enviando frame via DC: " + std::to_string(frame.width) + "x" + 
                std::to_string(frame.height) + " (orig:" + std::to_string(header.originalWidth) + "x" +
                std::to_string(header.originalHeight) + ") size=" + std::to_string(frame.payload.size()));
        }
        
        try {
            controlChannel_->send(packet);
            return true;
        } catch (...) {
            return false;
        }
    }

    void setInputHandlers(
        std::function<void(const vic::input::MouseEvent&)> mouseHandler,
        std::function<void(const vic::input::KeyboardEvent&)> keyboardHandler) {
        mouseHandler_ = std::move(mouseHandler);
        keyboardHandler_ = std::move(keyboardHandler);
        if (controlChannel_) {
            dataChannelWrapper_.attach(controlChannel_, mouseHandler_, keyboardHandler_);
        }
        if (fallbackServer_) {
            fallbackServer_->setInputHandlers(mouseHandler_, keyboardHandler_);
        }
    }

    void setConnectionStateCallback(std::function<void(ConnectionState)> callback) {
        stateCallback_ = std::move(callback);
        recomputeState();
    }

    void setConnectionInfo(const ConnectionInfo& info) {
        connectionCode_ = info.code;
        ensureFallbackInitialized();
        if (config_.tunnel && tunnelAgent_) {
            tunnelAgent_->start(info, config_.tunnel->localPort);
        }
    }

private:
    void setupCallbacks() {
        pc_->onStateChange([this](rtc::PeerConnection::State state) {
            peerState_.store(mapState(state));
            recomputeState();
        });

        pc_->onLocalDescription([this](rtc::Description description) {
            logging::global().log(logging::Logger::Level::Info, "TransportServer: local description produced");
            std::lock_guard lock(gatheringState_.mutex);
            gatheringState_.localDescription = toSessionDescription(description);
            gatheringState_.descriptionReady = true;
            gatheringState_.descriptionCv.notify_all();
        });

        pc_->onLocalCandidate([this](rtc::Candidate candidate) {
            logging::global().log(logging::Logger::Level::Debug, "TransportServer: new local ICE candidate");
            std::lock_guard lock(gatheringState_.mutex);
            gatheringState_.localCandidates.push_back(toIceCandidate(candidate));
        });

        pc_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                logging::global().log(logging::Logger::Level::Info, "TransportServer: ICE gathering complete");
                std::lock_guard lock(gatheringState_.mutex);
                gatheringState_.gatheringComplete = true;
                gatheringState_.gatheringCv.notify_all();
            }
        });

        pc_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
            controlChannel_ = std::move(channel);
            dataChannelWrapper_.attach(controlChannel_, mouseHandler_, keyboardHandler_);
        });
    }

    void setupMedia() {
        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        video.addVP8Codec(96);
        if (config_.ssrc != 0) {
            video.addSSRC(config_.ssrc, "vicviewer-video");
        }
        videoTrack_ = pc_->addTrack(std::move(video));
    }

    void setupDataChannel() {
        controlChannel_ = pc_->createDataChannel("vic-input");
        controlChannel_->onOpen([this]() {
            logging::global().log(logging::Logger::Level::Info, 
                "[Server] DataChannel ABIERTO - solicitando keyframe");
            needsKeyframe_.store(true, std::memory_order_release);
        });
        controlChannel_->onClosed([this]() {
            logging::global().log(logging::Logger::Level::Warning, 
                "[Server] DataChannel CERRADO");
        });
        dataChannelWrapper_.attach(controlChannel_, mouseHandler_, keyboardHandler_);
    }

    void ensureFallbackInitialized();
    void teardownFallback();
    void recomputeState();

    TransportConfig config_{};
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> videoTrack_;
    std::shared_ptr<rtc::DataChannel> controlChannel_;
    std::atomic<ConnectionState> state_{ConnectionState::New};
    std::function<void(ConnectionState)> stateCallback_;
    std::function<void(const vic::input::MouseEvent&)> mouseHandler_;
    std::function<void(const vic::input::KeyboardEvent&)> keyboardHandler_;
    DataChannelWrapper dataChannelWrapper_;
    LocalGatheringState gatheringState_;
    std::unique_ptr<fallback::Server> fallbackServer_;
    std::unique_ptr<TunnelAgent> tunnelAgent_;
    std::optional<std::string> connectionCode_;
    std::atomic_bool fallbackConnected_{false};
    std::atomic<ConnectionState> peerState_{ConnectionState::New};
    std::atomic_bool needsKeyframe_{false};
};

    void TransportServer::Impl::ensureFallbackInitialized() {
        if (!config_.tunnel) {
            return;
        }

        if (!fallbackServer_) {
            fallbackServer_ = std::make_unique<fallback::Server>();
            fallbackServer_->setInputHandlers(mouseHandler_, keyboardHandler_);
            fallbackServer_->setConnectionCallback([this](bool connected) {
                fallbackConnected_.store(connected, std::memory_order_release);
                recomputeState();
            });
            if (!fallbackServer_->start(config_.tunnel->localPort)) {
                logging::global().log(logging::Logger::Level::Warning,
                    "TransportServer: failed to start tunnel fallback server");
                fallbackServer_.reset();
            }
        } else {
            fallbackServer_->setInputHandlers(mouseHandler_, keyboardHandler_);
        }

        if (fallbackServer_ && !tunnelAgent_) {
            tunnelAgent_ = std::make_unique<TunnelAgent>(
                config_.tunnel->relayHost,
                config_.tunnel->controlPort,
                config_.tunnel->dataPort);
        }

        if (tunnelAgent_ && connectionCode_) {
            ConnectionInfo info{};
            info.code = *connectionCode_;
            tunnelAgent_->start(info, config_.tunnel->localPort);
        }
    }

    void TransportServer::Impl::teardownFallback() {
        if (tunnelAgent_) {
            tunnelAgent_->stop();
            tunnelAgent_.reset();
        }
        if (fallbackServer_) {
            fallbackServer_->stop();
            fallbackServer_.reset();
        }
        fallbackConnected_.store(false);
        connectionCode_.reset();
    }

    void TransportServer::Impl::recomputeState() {
        ConnectionState desired = peerState_.load();
        if (fallbackConnected_.load()) {
            desired = ConnectionState::Connected;
        }
        ConnectionState previous = state_.exchange(desired);
        if (stateCallback_ && previous != desired) {
            stateCallback_(desired);
        }
    }

class TransportClient::Impl {
public:
    bool start(const TransportConfig& config) {
        ensureRtcInitialized();
        stop();
        config_ = config;

        try {
            pc_ = std::make_shared<rtc::PeerConnection>(buildRtcConfiguration(config));
        } catch (const std::exception& ex) {
            logging::global().log(logging::Logger::Level::Error,
                std::string("TransportClient: failed to create PeerConnection: ") + ex.what());
            return false;
        }

        setupCallbacks();
        peerState_.store(ConnectionState::New);
        fallbackConnected_.store(false);
        state_.store(ConnectionState::New);
        ensureFallbackMonitor();
        recomputeState();
        return true;
    }

    AnswerBundle acceptOffer(const SessionDescription& offer) {
        if (!pc_) {
            throw std::runtime_error("TransportClient not started");
        }

        {
            std::lock_guard lock(gatheringState_.mutex);
            gatheringState_.descriptionReady = false;
            gatheringState_.gatheringComplete = false;
            gatheringState_.localCandidates.clear();
        }

        if (offer.sdp.find("a=ice-ufrag:") == std::string::npos) {
            logging::global().log(logging::Logger::Level::Warning,
                "TransportClient: oferta recibida sin credenciales ICE:\n" + offer.sdp);
        }

        try {
            rtc::Description description(offer.sdp, offer.type);
            pc_->setRemoteDescription(std::move(description));
        } catch (const std::exception& ex) {
            throw std::runtime_error(std::string("TransportClient: invalid offer: ") + ex.what());
        }

        pc_->setLocalDescription(rtc::Description::Type::Answer);

        std::unique_lock lock(gatheringState_.mutex);
        gatheringState_.descriptionCv.wait(lock, [this]() {
            return gatheringState_.descriptionReady;
        });

        gatheringState_.gatheringCv.wait_for(lock, std::chrono::seconds(5), [this]() {
            return gatheringState_.gatheringComplete;
        });

        AnswerBundle bundle;
        bundle.description = gatheringState_.localDescription;
        bundle.iceCandidates = gatheringState_.localCandidates;
        return bundle;
    }

    bool addRemoteCandidate(const IceCandidate& candidate) {
        if (!pc_) {
            return false;
        }
        try {
            pc_->addRemoteCandidate(fromIceCandidate(candidate));
            return true;
        } catch (const std::exception& ex) {
            logging::global().log(logging::Logger::Level::Warning,
                std::string("TransportClient: failed to add remote candidate: ") + ex.what());
            return false;
        }
    }

    void setFrameHandler(std::function<void(const vic::encoder::EncodedFrame&)> handler) {
        frameHandler_ = std::move(handler);
        // Re-attach DataChannel with updated frameHandler
        if (controlChannel_) {
            attachDataChannel();
        }
        if (fallbackClient_) {
            fallbackClient_->setFrameHandler([this](const vic::encoder::EncodedFrame& frame) {
                if (frameHandler_) {
                    frameHandler_(frame);
                }
            });
        }
    }

    void setConnectionStateCallback(std::function<void(ConnectionState)> callback) {
        stateCallback_ = std::move(callback);
        recomputeState();
    }

    void setConnectionInfo(const ConnectionInfo& info) {
        bool codeChanged = !connectionCode_ || *connectionCode_ != info.code;
        connectionCode_ = info.code;
        if (codeChanged) {
            std::lock_guard lock(fallbackMutex_);
            if (fallbackClient_ && fallbackClient_->isConnected()) {
                fallbackClient_->disconnect();
            }
        }
        ensureFallbackMonitor();
    }

    bool sendMouseEvent(const vic::input::MouseEvent& ev) {
        logging::global().log(logging::Logger::Level::Debug, 
            "[TransportClient] sendMouseEvent: x=" + std::to_string(ev.x) + " y=" + std::to_string(ev.y));
        if (dataChannelWrapper_.send(buildMousePayload(ev))) {
            logging::global().log(logging::Logger::Level::Debug, "[TransportClient] Mouse enviado via DataChannel OK");
            return true;
        }
        logging::global().log(logging::Logger::Level::Debug, "[TransportClient] DataChannel no disponible, intentando fallback");
        std::lock_guard lock(fallbackMutex_);
        if (fallbackClient_ && fallbackClient_->isConnected()) {
            logging::global().log(logging::Logger::Level::Debug, "[TransportClient] Usando fallback para mouse");
            return fallbackClient_->sendMouseEvent(ev);
        }
        logging::global().log(logging::Logger::Level::Warning, "[TransportClient] No se pudo enviar evento de mouse");
        return false;
    }

    bool sendKeyboardEvent(const vic::input::KeyboardEvent& ev) {
        if (dataChannelWrapper_.send(buildKeyboardPayload(ev))) {
            return true;
        }
        std::lock_guard lock(fallbackMutex_);
        if (fallbackClient_ && fallbackClient_->isConnected()) {
            return fallbackClient_->sendKeyboardEvent(ev);
        }
        return false;
    }

    void stop() {
        if (pc_) {
            pc_->close();
        }
        pc_.reset();
        controlChannel_.reset();
        videoTrack_.reset();
        currentWidth_ = 0;
        currentHeight_ = 0;
        {
            std::lock_guard lock(gatheringState_.mutex);
            gatheringState_.descriptionReady = false;
            gatheringState_.gatheringComplete = false;
            gatheringState_.localCandidates.clear();
        }
        stopFallback();
        connectionCode_.reset();
        fallbackConnected_.store(false);
        peerState_.store(ConnectionState::Closed);
        state_.store(ConnectionState::Closed);
        recomputeState();
    }

private:
    void setupCallbacks() {
        pc_->onStateChange([this](rtc::PeerConnection::State state) {
            std::string stateStr;
            switch (state) {
                case rtc::PeerConnection::State::New: stateStr = "New"; break;
                case rtc::PeerConnection::State::Connecting: stateStr = "Connecting"; break;
                case rtc::PeerConnection::State::Connected: stateStr = "Connected"; break;
                case rtc::PeerConnection::State::Disconnected: stateStr = "Disconnected"; break;
                case rtc::PeerConnection::State::Failed: stateStr = "Failed"; break;
                case rtc::PeerConnection::State::Closed: stateStr = "Closed"; break;
                default: stateStr = "Unknown"; break;
            }
            logging::global().log(logging::Logger::Level::Info, 
                "[TransportClient] Estado PeerConnection: " + stateStr);
            peerState_.store(mapState(state));
            recomputeState();
        });

        pc_->onIceStateChange([this](rtc::PeerConnection::IceState state) {
            std::string stateStr;
            switch (state) {
                case rtc::PeerConnection::IceState::New: stateStr = "New"; break;
                case rtc::PeerConnection::IceState::Checking: stateStr = "Checking"; break;
                case rtc::PeerConnection::IceState::Connected: stateStr = "Connected"; break;
                case rtc::PeerConnection::IceState::Completed: stateStr = "Completed"; break;
                case rtc::PeerConnection::IceState::Failed: stateStr = "Failed"; break;
                case rtc::PeerConnection::IceState::Disconnected: stateStr = "Disconnected"; break;
                case rtc::PeerConnection::IceState::Closed: stateStr = "Closed"; break;
                default: stateStr = "Unknown"; break;
            }
            logging::global().log(logging::Logger::Level::Info, 
                "[TransportClient] Estado ICE: " + stateStr);
        });

        pc_->onLocalDescription([this](rtc::Description description) {
            std::lock_guard lock(gatheringState_.mutex);
            gatheringState_.localDescription = toSessionDescription(description);
            gatheringState_.descriptionReady = true;
            gatheringState_.descriptionCv.notify_all();
        });

        pc_->onLocalCandidate([this](rtc::Candidate candidate) {
            std::lock_guard lock(gatheringState_.mutex);
            gatheringState_.localCandidates.push_back(toIceCandidate(candidate));
        });

        pc_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                std::lock_guard lock(gatheringState_.mutex);
                gatheringState_.gatheringComplete = true;
                gatheringState_.gatheringCv.notify_all();
            }
        });

        pc_->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel) {
            logging::global().log(logging::Logger::Level::Info, 
                "[TransportClient] DataChannel recibido: " + channel->label());
            controlChannel_ = std::move(channel);
            controlChannel_->onOpen([this]() {
                logging::global().log(logging::Logger::Level::Info, "[TransportClient] DataChannel ABIERTO - listo para enviar");
            });
            controlChannel_->onClosed([this]() {
                logging::global().log(logging::Logger::Level::Warning, "[TransportClient] DataChannel CERRADO");
            });
            attachDataChannel();
        });

        pc_->onTrack([this](std::shared_ptr<rtc::Track> track) {
            if (!track) {
                return;
            }
            videoTrack_ = std::move(track);
            videoTrack_->onFrame([this](rtc::binary data, rtc::FrameInfo info) {
                handleIncomingFrame(std::move(data), info);
            });
        });
    }

    void attachDataChannel() {
        // Pass frameHandler to DataChannel for video-over-datachannel fallback
        dataChannelWrapper_.attach(controlChannel_, nullptr, nullptr, frameHandler_);
    }

    void handleIncomingFrame(rtc::binary data, const rtc::FrameInfo& info) {
        if (!frameHandler_) {
            return;
        }

        Vp8Metadata metadata = parseVp8Metadata(data, currentWidth_, currentHeight_);
        if (metadata.keyFrame) {
            currentWidth_ = metadata.width;
            currentHeight_ = metadata.height;
        }

        vic::encoder::EncodedFrame frame{};
        frame.timestamp = extractTimestampMs(info, config_.clockRate);
        frame.width = currentWidth_;
        frame.height = currentHeight_;
        frame.keyFrame = metadata.keyFrame;
        frame.payload.resize(data.size());
        std::transform(data.begin(), data.end(), frame.payload.begin(), [](std::byte b) {
            return std::to_integer<uint8_t>(b);
        });

        frameHandler_(frame);
    }

    void ensureFallbackMonitor();
    void stopFallback();
    void monitorFallback();
    void recomputeState();

    TransportConfig config_{};
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::shared_ptr<rtc::Track> videoTrack_;
    std::shared_ptr<rtc::DataChannel> controlChannel_;
    DataChannelWrapper dataChannelWrapper_;
    std::function<void(const vic::encoder::EncodedFrame&)> frameHandler_;
    std::function<void(ConnectionState)> stateCallback_;
    LocalGatheringState gatheringState_;
    uint32_t currentWidth_{0};
    uint32_t currentHeight_{0};
    std::unique_ptr<fallback::Client> fallbackClient_;
    std::optional<std::string> connectionCode_;
    std::atomic_bool fallbackConnected_{false};
    std::atomic<ConnectionState> peerState_{ConnectionState::New};
    std::atomic<ConnectionState> state_{ConnectionState::New};
    std::thread fallbackMonitorThread_;
    std::atomic_bool fallbackMonitorRunning_{false};
    std::mutex fallbackMutex_;
};

    void TransportClient::Impl::ensureFallbackMonitor() {
        if (!config_.tunnel || !connectionCode_) {
            return;
        }

        {
            std::lock_guard lock(fallbackMutex_);
            if (!fallbackClient_) {
                fallbackClient_ = std::make_unique<fallback::Client>();
                fallbackClient_->setConnectionCallback([this](bool connected) {
                    fallbackConnected_.store(connected);
                    recomputeState();
                });
            }
            if (fallbackClient_) {
                fallbackClient_->setFrameHandler([this](const vic::encoder::EncodedFrame& frame) {
                    if (frameHandler_) {
                        frameHandler_(frame);
                    }
                });
            }
        }

        if (!fallbackMonitorRunning_.exchange(true)) {
            fallbackMonitorThread_ = std::thread(&TransportClient::Impl::monitorFallback, this);
        }
    }

    void TransportClient::Impl::stopFallback() {
        if (fallbackMonitorRunning_.exchange(false)) {
            if (fallbackMonitorThread_.joinable()) {
                fallbackMonitorThread_.join();
            }
        }
        std::lock_guard lock(fallbackMutex_);
        if (fallbackClient_) {
            fallbackClient_->disconnect();
            fallbackClient_.reset();
        }
        fallbackConnected_.store(false);
    }

    void TransportClient::Impl::monitorFallback() {
        using namespace std::chrono_literals;
        while (fallbackMonitorRunning_.load()) {
            auto tunnel = config_.tunnel;
            auto code = connectionCode_;
            if (!tunnel || !code) {
                std::this_thread::sleep_for(1s);
                continue;
            }

            ConnectionState peer = peerState_.load();
            bool needConnect = false;
            bool needDisconnect = false;
            bool hasClient = false;

            {
                std::lock_guard lock(fallbackMutex_);
                if (!fallbackClient_) {
                    hasClient = false;
                } else {
                    hasClient = true;
                    if (peer == ConnectionState::Connected) {
                        if (fallbackClient_->isConnected()) {
                            needDisconnect = true;
                        }
                    } else {
                        if (!fallbackClient_->isConnected()) {
                            needConnect = true;
                        }
                    }
                }
            }

            if (!hasClient) {
                std::this_thread::sleep_for(1s);
                continue;
            }

            if (needDisconnect) {
                std::lock_guard lock(fallbackMutex_);
                if (fallbackClient_) {
                    fallbackClient_->disconnect();
                }
            } else if (needConnect) {
                bool success = false;
                {
                    std::lock_guard lock(fallbackMutex_);
                    if (fallbackClient_) {
                        success = fallbackClient_->connect(*tunnel, *code);
                    }
                }
                if (!success) {
                    std::this_thread::sleep_for(2s);
                    continue;
                }
            }

            std::this_thread::sleep_for(1s);
        }
    }

    void TransportClient::Impl::recomputeState() {
        ConnectionState desired = peerState_.load();
        if (fallbackConnected_.load()) {
            desired = ConnectionState::Connected;
        }
        ConnectionState previous = state_.exchange(desired);
        if (stateCallback_ && previous != desired) {
            stateCallback_(desired);
        }
    }

TransportServer::TransportServer()
    : impl_(std::make_unique<Impl>()) {}

TransportServer::~TransportServer() = default;

bool TransportServer::start(const TransportConfig& config) {
    return impl_->start(config);
}

void TransportServer::stop() {
    impl_->stop();
}

OfferBundle TransportServer::createOfferBundle() {
    return impl_->createOfferBundle();
}

bool TransportServer::applyAnswer(const SessionDescription& answer) {
    return impl_->applyAnswer(answer);
}

bool TransportServer::addRemoteCandidate(const IceCandidate& candidate) {
    return impl_->addRemoteCandidate(candidate);
}

bool TransportServer::sendFrame(const vic::encoder::EncodedFrame& frame) {
    return impl_->sendFrame(frame);
}

bool TransportServer::needsInitialKeyframe() {
    return impl_->needsInitialKeyframe();
}

void TransportServer::setInputHandlers(
    std::function<void(const vic::input::MouseEvent&)> mouseHandler,
    std::function<void(const vic::input::KeyboardEvent&)> keyboardHandler) {
    impl_->setInputHandlers(std::move(mouseHandler), std::move(keyboardHandler));
}

void TransportServer::setConnectionInfo(const ConnectionInfo& info) {
    impl_->setConnectionInfo(info);
}

void TransportServer::setConnectionStateCallback(std::function<void(ConnectionState)> callback) {
    impl_->setConnectionStateCallback(std::move(callback));
}

TransportClient::TransportClient()
    : impl_(std::make_unique<Impl>()) {}

TransportClient::~TransportClient() = default;

bool TransportClient::start(const TransportConfig& config) {
    return impl_->start(config);
}

AnswerBundle TransportClient::acceptOffer(const SessionDescription& offer) {
    return impl_->acceptOffer(offer);
}

bool TransportClient::addRemoteCandidate(const IceCandidate& candidate) {
    return impl_->addRemoteCandidate(candidate);
}

void TransportClient::setFrameHandler(std::function<void(const vic::encoder::EncodedFrame&)> handler) {
    impl_->setFrameHandler(std::move(handler));
}

void TransportClient::setConnectionStateCallback(std::function<void(ConnectionState)> callback) {
    impl_->setConnectionStateCallback(std::move(callback));
}

void TransportClient::setConnectionInfo(const ConnectionInfo& info) {
    impl_->setConnectionInfo(info);
}

bool TransportClient::sendMouseEvent(const vic::input::MouseEvent& ev) {
    return impl_->sendMouseEvent(ev);
}

bool TransportClient::sendKeyboardEvent(const vic::input::KeyboardEvent& ev) {
    return impl_->sendKeyboardEvent(ev);
}

void TransportClient::stop() {
    impl_->stop();
}

} // namespace vic::transport
