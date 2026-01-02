#include "TunnelFallback.h"

#include "Logger.h"

#include <Ws2tcpip.h>

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace vic::transport::fallback {

namespace {

constexpr uint8_t kFrameMessageType = 0x10;
constexpr size_t kHeaderSize = 5;
constexpr size_t kFrameMetaSize = sizeof(uint64_t) + sizeof(uint32_t) * 3 + sizeof(uint8_t);
constexpr size_t kFrameMinimumSize = sizeof(uint64_t) + sizeof(uint32_t) * 2 + sizeof(uint8_t) + sizeof(uint32_t);
constexpr size_t kMaxPayloadSize = 16 * 1024 * 1024; // 16 MB safety cap

bool ensureWinsockInitialized() {
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, []() {
        WSADATA data{};
        initialized = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!initialized) {
            logging::global().log(logging::Logger::Level::Error, "TunnelFallback: WSAStartup failed");
        }
    });
    return initialized;
}

void writeUint32(uint8_t* dest, uint32_t value) {
    dest[0] = static_cast<uint8_t>(value & 0xFF);
    dest[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dest[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dest[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

uint32_t readUint32(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) |
        (static_cast<uint32_t>(src[1]) << 8) |
        (static_cast<uint32_t>(src[2]) << 16) |
        (static_cast<uint32_t>(src[3]) << 24);
}

void writeUint64(uint8_t* dest, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        dest[i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
}

uint64_t readUint64(const uint8_t* src) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(src[i]) << (i * 8);
    }
    return value;
}

bool sendLine(SOCKET socket, const std::string& line) {
    std::string payload = line;
    payload.push_back('\n');
    size_t total = 0;
    while (total < payload.size()) {
        int sent = send(socket, payload.data() + total, static_cast<int>(payload.size() - total), 0);
        if (sent <= 0) {
            return false;
        }
        total += static_cast<size_t>(sent);
    }
    return true;
}

std::optional<std::string> readLine(SOCKET socket) {
    std::string line;
    line.reserve(128);
    char ch = 0;
    while (true) {
        int received = recv(socket, &ch, 1, 0);
        if (received <= 0) {
            return std::nullopt;
        }
        if (ch == '\n') {
            break;
        }
        line.push_back(ch);
        if (line.size() > 256) {
            return std::nullopt;
        }
    }
    return line;
}

bool isFrameHeaderValid(uint32_t size) {
    if (size < kFrameMinimumSize) {
        return false;
    }
    if (size > kMaxPayloadSize + kFrameMetaSize) {
        return false;
    }
    return true;
}

} // namespace

Server::Server() = default;

Server::~Server() {
    stop();
}

bool Server::start(uint16_t port) {
    if (running_.load()) {
        return true;
    }
    if (!ensureWinsockInitialized()) {
        return false;
    }

    listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket_ == INVALID_SOCKET) {
        logging::global().log(logging::Logger::Level::Error, "TunnelFallback: failed to create listen socket");
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    BOOL reuse = TRUE;
    setsockopt(listenSocket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (bind(listenSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        logging::global().log(logging::Logger::Level::Error, "TunnelFallback: bind failed");
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    if (listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
        logging::global().log(logging::Logger::Level::Error, "TunnelFallback: listen failed");
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
        return false;
    }

    stopRequested_.store(false);
    running_.store(true);
    acceptThread_ = std::thread(&Server::acceptLoop, this);
    logging::global().log(logging::Logger::Level::Info, "TunnelFallback: server listening on localhost:" + std::to_string(port));
    return true;
}

void Server::stop() {
    stopRequested_.store(true);
    running_.store(false);

    if (listenSocket_ != INVALID_SOCKET) {
        shutdown(listenSocket_, SD_BOTH);
        closesocket(listenSocket_);
        listenSocket_ = INVALID_SOCKET;
    }

    {
        std::lock_guard lock(clientMutex_);
        if (clientSocket_ != INVALID_SOCKET) {
            shutdown(clientSocket_, SD_BOTH);
            closesocket(clientSocket_);
            clientSocket_ = INVALID_SOCKET;
        }
    }

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    if (clientThread_.joinable()) {
        clientThread_.join();
    }

    clientConnected_.store(false);
}

void Server::setInputHandlers(
    std::function<void(const vic::input::MouseEvent&)> mouseHandler,
    std::function<void(const vic::input::KeyboardEvent&)> keyboardHandler) {
    mouseHandler_ = std::move(mouseHandler);
    keyboardHandler_ = std::move(keyboardHandler);
}

void Server::setConnectionCallback(std::function<void(bool)> callback) {
    connectionCallback_ = std::move(callback);
}

bool Server::sendFrame(const vic::encoder::EncodedFrame& frame) {
    SOCKET socket = INVALID_SOCKET;
    {
        std::lock_guard lock(clientMutex_);
        socket = clientSocket_;
    }
    if (socket == INVALID_SOCKET) {
        return false;
    }
    std::lock_guard sendLock(sendMutex_);
    return sendFrameInternal(socket, frame);
}

bool Server::hasClient() const {
    return clientConnected_.load();
}

void Server::acceptLoop() {
    while (!stopRequested_.load()) {
        SOCKET client = accept(listenSocket_, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (stopRequested_.load()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        SOCKET previous = INVALID_SOCKET;
        {
            std::lock_guard lock(clientMutex_);
            previous = clientSocket_;
            clientSocket_ = client;
        }

        if (previous != INVALID_SOCKET) {
            shutdown(previous, SD_BOTH);
            closesocket(previous);
        }

        if (clientThread_.joinable()) {
            clientThread_.join();
        }

        clientConnected_.store(true);
        if (connectionCallback_) {
            connectionCallback_(true);
        }
        clientThread_ = std::thread(&Server::clientLoop, this);
    }
}

void Server::clientLoop() {
    SOCKET socket = INVALID_SOCKET;
    {
        std::lock_guard lock(clientMutex_);
        socket = clientSocket_;
    }
    if (socket == INVALID_SOCKET) {
        return;
    }

    std::array<uint8_t, kHeaderSize> header{};
    while (!stopRequested_.load()) {
        if (!recvAll(socket, header.data(), header.size())) {
            break;
        }
        uint8_t type = header[0];
        uint32_t size = readUint32(header.data() + 1);
        if (size == 0 || size > 1024) {
            // Input events are small; treat larger payload as protocol error.
            break;
        }
        std::vector<uint8_t> payload(size);
        if (!recvAll(socket, payload.data(), payload.size())) {
            break;
        }
        handleClientMessage(type, payload);
    }

    clientConnected_.store(false);
    if (connectionCallback_) {
        connectionCallback_(false);
    }

    {
        std::lock_guard lock(clientMutex_);
        if (clientSocket_ == socket) {
            clientSocket_ = INVALID_SOCKET;
        }
    }
    shutdown(socket, SD_BOTH);
    closesocket(socket);
}

void Server::handleClientMessage(uint8_t type, const std::vector<uint8_t>& payload) {
    if (type == static_cast<uint8_t>(protocol::ControlMessageType::Mouse)) {
        if (payload.size() != sizeof(protocol::MouseMessage)) {
            return;
        }
        protocol::MouseMessage msg{};
        std::memcpy(&msg, payload.data(), sizeof(protocol::MouseMessage));
        if (mouseHandler_) {
            vic::input::MouseEvent ev{};
            ev.x = msg.x;
            ev.y = msg.y;
            ev.wheelDelta = msg.wheel;
            ev.action = static_cast<vic::input::MouseAction>(msg.action);
            ev.button = static_cast<vic::input::MouseButton>(msg.button);
            mouseHandler_(ev);
        }
    } else if (type == static_cast<uint8_t>(protocol::ControlMessageType::Keyboard)) {
        if (payload.size() != sizeof(protocol::KeyboardMessage)) {
            return;
        }
        protocol::KeyboardMessage msg{};
        std::memcpy(&msg, payload.data(), sizeof(protocol::KeyboardMessage));
        if (keyboardHandler_) {
            vic::input::KeyboardEvent ev{};
            ev.virtualKey = msg.vk;
            ev.scanCode = msg.scan;
            ev.action = static_cast<vic::input::KeyAction>(msg.action);
            keyboardHandler_(ev);
        }
    }
}

bool Server::sendAll(SOCKET socket, const void* data, size_t length) {
    const char* ptr = static_cast<const char*>(data);
    size_t total = 0;
    while (total < length) {
        int sent = send(socket, ptr + total, static_cast<int>(length - total), 0);
        if (sent <= 0) {
            return false;
        }
        total += static_cast<size_t>(sent);
    }
    return true;
}

bool Server::recvAll(SOCKET socket, void* data, size_t length) {
    char* ptr = static_cast<char*>(data);
    size_t total = 0;
    while (total < length) {
        int received = recv(socket, ptr + total, static_cast<int>(length - total), 0);
        if (received <= 0) {
            return false;
        }
        total += static_cast<size_t>(received);
    }
    return true;
}

bool Server::writeHeader(SOCKET socket, uint8_t type, uint32_t size) {
    std::array<uint8_t, kHeaderSize> header{};
    header[0] = type;
    writeUint32(header.data() + 1, size);
    return sendAll(socket, header.data(), header.size());
}

bool Server::sendFrameInternal(SOCKET socket, const vic::encoder::EncodedFrame& frame) {
    const uint32_t payloadSize = static_cast<uint32_t>(frame.payload.size());
    const uint32_t messageSize = static_cast<uint32_t>(sizeof(uint64_t) + sizeof(uint32_t) * 3 + sizeof(uint8_t) + payloadSize);
    if (!writeHeader(socket, kFrameMessageType, messageSize)) {
        return false;
    }

    std::array<uint8_t, sizeof(uint64_t) + sizeof(uint32_t) * 3 + sizeof(uint8_t)> meta{};
    writeUint64(meta.data(), frame.timestamp);
    writeUint32(meta.data() + 8, frame.width);
    writeUint32(meta.data() + 12, frame.height);
    meta[16] = frame.keyFrame ? 1 : 0;
    writeUint32(meta.data() + 17, payloadSize);

    if (!sendAll(socket, meta.data(), meta.size())) {
        return false;
    }
    if (payloadSize > 0) {
        if (!sendAll(socket, frame.payload.data(), payloadSize)) {
            return false;
        }
    }
    return true;
}

Client::Client() = default;

Client::~Client() {
    disconnect();
}

void Client::setFrameHandler(std::function<void(const vic::encoder::EncodedFrame&)> handler) {
    frameHandler_ = std::move(handler);
}

void Client::setConnectionCallback(std::function<void(bool)> callback) {
    connectionCallback_ = std::move(callback);
}

bool Client::connect(const TunnelConfig& config, const std::string& code) {
    if (code.empty()) {
        return false;
    }
    if (!ensureWinsockInitialized()) {
        return false;
    }

    disconnect();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    std::string portStr = std::to_string(config.dataPort);
    if (getaddrinfo(config.relayHost.c_str(), portStr.c_str(), &hints, &result) != 0) {
        return false;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == INVALID_SOCKET) {
            continue;
        }
    if (::connect(sock, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == SOCKET_ERROR) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            continue;
        }
        break;
    }
    freeaddrinfo(result);

    if (sock == INVALID_SOCKET) {
        return false;
    }

    if (!sendLine(sock, "VIEWER code=" + code)) {
        closesocket(sock);
        return false;
    }

    auto waitLine = readLine(sock);
    if (!waitLine) {
        closesocket(sock);
        return false;
    }
    if (waitLine->rfind("ERR", 0) == 0) {
        closesocket(sock);
        return false;
    }

    auto okLine = readLine(sock);
    if (!okLine || okLine->rfind("OK", 0) != 0) {
        closesocket(sock);
        return false;
    }

    {
        std::lock_guard lock(socketMutex_);
        socket_ = sock;
    }
    stopRequested_.store(false);
    connected_.store(true);
    if (connectionCallback_) {
        connectionCallback_(true);
    }
    receiveThread_ = std::thread(&Client::receiveLoop, this);
    return true;
}

void Client::disconnect() {
    stopRequested_.store(true);
    SOCKET socket = INVALID_SOCKET;
    {
        std::lock_guard lock(socketMutex_);
        socket = socket_;
        socket_ = INVALID_SOCKET;
    }
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
    if (receiveThread_.joinable()) {
        receiveThread_.join();
    }
    if (connected_.exchange(false)) {
        if (connectionCallback_) {
            connectionCallback_(false);
        }
    }
    stopRequested_.store(false);
}

bool Client::sendMouseEvent(const vic::input::MouseEvent& ev) {
    SOCKET socket = INVALID_SOCKET;
    {
        std::lock_guard lock(socketMutex_);
        socket = socket_;
    }
    if (socket == INVALID_SOCKET) {
        return false;
    }

    protocol::MouseMessage msg{};
    msg.x = ev.x;
    msg.y = ev.y;
    msg.wheel = ev.wheelDelta;
    msg.action = static_cast<uint8_t>(ev.action);
    msg.button = static_cast<uint8_t>(ev.button);

    std::lock_guard sendLock(sendMutex_);
    if (!writeHeader(socket, static_cast<uint8_t>(protocol::ControlMessageType::Mouse), sizeof(protocol::MouseMessage))) {
        return false;
    }
    return sendAll(socket, &msg, sizeof(protocol::MouseMessage));
}

bool Client::sendKeyboardEvent(const vic::input::KeyboardEvent& ev) {
    SOCKET socket = INVALID_SOCKET;
    {
        std::lock_guard lock(socketMutex_);
        socket = socket_;
    }
    if (socket == INVALID_SOCKET) {
        return false;
    }

    protocol::KeyboardMessage msg{};
    msg.vk = ev.virtualKey;
    msg.scan = ev.scanCode;
    msg.action = static_cast<uint8_t>(ev.action);

    std::lock_guard sendLock(sendMutex_);
    if (!writeHeader(socket, static_cast<uint8_t>(protocol::ControlMessageType::Keyboard), sizeof(protocol::KeyboardMessage))) {
        return false;
    }
    return sendAll(socket, &msg, sizeof(protocol::KeyboardMessage));
}

bool Client::isConnected() const {
    return connected_.load();
}

void Client::receiveLoop() {
    SOCKET socket = INVALID_SOCKET;
    {
        std::lock_guard lock(socketMutex_);
        socket = socket_;
    }
    if (socket == INVALID_SOCKET) {
        return;
    }

    std::array<uint8_t, kHeaderSize> header{};
    while (!stopRequested_.load()) {
        if (!recvAll(socket, header.data(), header.size())) {
            break;
        }
        uint8_t type = header[0];
        uint32_t size = readUint32(header.data() + 1);
        if (type != kFrameMessageType) {
            // Ignore unknown types.
            if (size > 0) {
                std::vector<uint8_t> skip(size);
                if (!recvAll(socket, skip.data(), skip.size())) {
                    break;
                }
            }
            continue;
        }
        if (!isFrameHeaderValid(size)) {
            break;
        }
        std::vector<uint8_t> payload(size);
        if (!recvAll(socket, payload.data(), payload.size())) {
            break;
        }
        if (!handleFramePayload(payload)) {
            break;
        }
    }

    connected_.store(false);
    if (connectionCallback_) {
        connectionCallback_(false);
    }

    {
        std::lock_guard lock(socketMutex_);
        if (socket_ == socket) {
            socket_ = INVALID_SOCKET;
        }
    }
    shutdown(socket, SD_BOTH);
    closesocket(socket);
}

bool Client::handleFramePayload(const std::vector<uint8_t>& payload) {
    if (payload.size() < kFrameMinimumSize) {
        return false;
    }

    size_t offset = 0;
    uint64_t timestamp = readUint64(payload.data() + offset);
    offset += sizeof(uint64_t);
    uint32_t width = readUint32(payload.data() + offset);
    offset += sizeof(uint32_t);
    uint32_t height = readUint32(payload.data() + offset);
    offset += sizeof(uint32_t);
    bool keyFrame = payload[offset] != 0;
    offset += sizeof(uint8_t);
    uint32_t frameSize = readUint32(payload.data() + offset);
    offset += sizeof(uint32_t);

    if (offset + frameSize > payload.size()) {
        return false;
    }
    if (frameSize > kMaxPayloadSize) {
        return false;
    }

    vic::encoder::EncodedFrame frame{};
    frame.timestamp = timestamp;
    frame.width = width;
    frame.height = height;
    frame.keyFrame = keyFrame;
    frame.payload.resize(frameSize);
    if (frameSize > 0) {
        std::memcpy(frame.payload.data(), payload.data() + offset, frameSize);
    }

    if (frameHandler_) {
        frameHandler_(frame);
    }
    return true;
}

bool Client::sendAll(SOCKET socket, const void* data, size_t length) {
    const char* ptr = static_cast<const char*>(data);
    size_t total = 0;
    while (total < length) {
        int sent = send(socket, ptr + total, static_cast<int>(length - total), 0);
        if (sent <= 0) {
            return false;
        }
        total += static_cast<size_t>(sent);
    }
    return true;
}

bool Client::recvAll(SOCKET socket, void* data, size_t length) {
    char* ptr = static_cast<char*>(data);
    size_t total = 0;
    while (total < length) {
        int received = recv(socket, ptr + total, static_cast<int>(length - total), 0);
        if (received <= 0) {
            return false;
        }
        total += static_cast<size_t>(received);
    }
    return true;
}

bool Client::writeHeader(SOCKET socket, uint8_t type, uint32_t size) {
    std::array<uint8_t, kHeaderSize> header{};
    header[0] = type;
    writeUint32(header.data() + 1, size);
    return sendAll(socket, header.data(), header.size());
}

} // namespace vic::transport::fallback
