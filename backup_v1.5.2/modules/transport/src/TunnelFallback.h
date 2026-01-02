#pragma once

#include "EncodedFrame.h"
#include "InputEvents.h"
#include "Transport.h"
#include "TransportProtocol.h"

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace vic::transport::fallback {

class Server {
public:
    Server();
    ~Server();

    bool start(uint16_t port);
    void stop();

    bool sendFrame(const vic::encoder::EncodedFrame& frame);

    void setInputHandlers(
        std::function<void(const vic::input::MouseEvent&)> mouseHandler,
        std::function<void(const vic::input::KeyboardEvent&)> keyboardHandler);
    void setConnectionCallback(std::function<void(bool)> callback);

    bool hasClient() const;

private:
    void acceptLoop();
    void clientLoop();

    static bool sendAll(SOCKET socket, const void* data, size_t length);
    static bool recvAll(SOCKET socket, void* data, size_t length);

    bool sendFrameInternal(SOCKET socket, const vic::encoder::EncodedFrame& frame);

    void handleClientMessage(uint8_t type, const std::vector<uint8_t>& payload);

    static bool writeHeader(SOCKET socket, uint8_t type, uint32_t size);

    std::function<void(const vic::input::MouseEvent&)> mouseHandler_{};
    std::function<void(const vic::input::KeyboardEvent&)> keyboardHandler_{};
    std::function<void(bool)> connectionCallback_{};

    std::atomic_bool running_{false};
    std::atomic_bool stopRequested_{false};
    std::atomic_bool clientConnected_{false};

    SOCKET listenSocket_{INVALID_SOCKET};
    SOCKET clientSocket_{INVALID_SOCKET};

    std::thread acceptThread_;
    std::thread clientThread_;

    mutable std::mutex sendMutex_;
    mutable std::mutex clientMutex_;
};

class Client {
public:
    Client();
    ~Client();

    void setFrameHandler(std::function<void(const vic::encoder::EncodedFrame&)> handler);
    void setConnectionCallback(std::function<void(bool)> callback);

    bool connect(const TunnelConfig& config, const std::string& code);
    void disconnect();

    bool sendMouseEvent(const vic::input::MouseEvent& ev);
    bool sendKeyboardEvent(const vic::input::KeyboardEvent& ev);

    bool isConnected() const;

private:
    void receiveLoop();

    static bool sendAll(SOCKET socket, const void* data, size_t length);
    static bool recvAll(SOCKET socket, void* data, size_t length);
    static bool writeHeader(SOCKET socket, uint8_t type, uint32_t size);

    bool handleFramePayload(const std::vector<uint8_t>& payload);

    std::function<void(const vic::encoder::EncodedFrame&)> frameHandler_{};
    std::function<void(bool)> connectionCallback_{};

    std::atomic_bool connected_{false};
    std::atomic_bool stopRequested_{false};

    SOCKET socket_{INVALID_SOCKET};
    std::thread receiveThread_;
    mutable std::mutex sendMutex_;
    mutable std::mutex socketMutex_;
};

} // namespace vic::transport::fallback
