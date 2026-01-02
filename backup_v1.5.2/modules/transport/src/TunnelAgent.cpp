#include "TunnelAgent.h"

#include "Logger.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <chrono>
#include <sstream>

namespace {
constexpr std::chrono::milliseconds kReconnectDelay{2000};
constexpr size_t kMaxLine = 256;

bool sendLine(SOCKET socket, const std::string& line) {
    std::string payload = line + "\n";
    int sent = send(socket, payload.c_str(), static_cast<int>(payload.size()), 0);
    return sent == static_cast<int>(payload.size());
}

std::optional<std::string> readLine(SOCKET socket) {
    std::string line;
    line.reserve(128);
    char ch;
    while (true) {
        int ret = recv(socket, &ch, 1, 0);
        if (ret <= 0) {
            return std::nullopt;
        }
        if (ch == '\n') {
            return line;
        }
        if (line.size() >= kMaxLine) {
            return std::nullopt;
        }
        line.push_back(ch);
    }
}

std::vector<std::string> splitTokens(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> out;
    std::string token;
    while (iss >> token) {
        out.push_back(token);
    }
    return out;
}

std::string getValue(const std::vector<std::string>& tokens, const std::string& key) {
    std::string prefix = key + "=";
    for (const auto& token : tokens) {
        if (token.rfind(prefix, 0) == 0) {
            return token.substr(prefix.size());
        }
    }
    return {};
}

} // namespace

namespace vic::transport {

TunnelAgent::TunnelAgent(std::string relayHost, uint16_t controlPort, uint16_t dataPort)
    : relayHost_(std::move(relayHost)),
      controlPort_(controlPort),
      dataPort_(dataPort) {
}

TunnelAgent::~TunnelAgent() {
    stop();
}

bool TunnelAgent::start(const ConnectionInfo& info, uint16_t localPort) {
    updateConnection(info);
    localPort_.store(localPort, std::memory_order_release);
    if (running_.load()) {
        return true;
    }
    stopRequested_.store(false);
    running_.store(true);
    controlThread_ = std::thread(&TunnelAgent::controlLoop, this);
    return true;
}

void TunnelAgent::stop() {
    stopRequested_.store(true);
    running_.store(false);
    if (controlThread_.joinable()) {
        controlThread_.join();
    }
}

void TunnelAgent::updateConnection(const ConnectionInfo& info) {
    std::lock_guard guard(codeMutex_);
    code_ = info.code;
}

void TunnelAgent::controlLoop() {
    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        logging::global().log(logging::Logger::Level::Error, "TunnelAgent: WSAStartup failed");
        return;
    }

    while (!stopRequested_.load()) {
        SOCKET controlSocket = connectToRelay(controlPort_);
        if (controlSocket == INVALID_SOCKET) {
            std::this_thread::sleep_for(kReconnectDelay);
            continue;
        }
        std::string code;
        {
            std::lock_guard guard(codeMutex_);
            code = code_;
        }
        if (!sendLine(controlSocket, "HOST code=" + code)) {
            closesocket(controlSocket);
            std::this_thread::sleep_for(kReconnectDelay);
            continue;
        }
        auto response = readLine(controlSocket);
        if (!response || response->rfind("OK", 0) != 0) {
            closesocket(controlSocket);
            std::this_thread::sleep_for(kReconnectDelay);
            continue;
        }
        logging::global().log(logging::Logger::Level::Info, "TunnelAgent: Control conectado");
        controlConnected_.store(true);

        while (!stopRequested_.load()) {
            auto lineOpt = readLine(controlSocket);
            if (!lineOpt) {
                break;
            }
            auto tokens = splitTokens(*lineOpt);
            if (tokens.empty()) {
                continue;
            }
            if (tokens[0] == "NEW") {
                auto channel = getValue(tokens, "channel");
                if (!channel.empty()) {
                    launchBridge(channel);
                }
            }
        }
        controlConnected_.store(false);
        closesocket(controlSocket);
        if (!stopRequested_.load()) {
            std::this_thread::sleep_for(kReconnectDelay);
        }
    }

    WSACleanup();
}

void TunnelAgent::handleControlMessage(const std::string& line) {
    (void)line;
}

void TunnelAgent::launchBridge(const std::string& channelId) {
    std::thread(&TunnelAgent::bridgeLoop, this, channelId).detach();
}

void TunnelAgent::bridgeLoop(const std::string& channelId) {
    SOCKET relaySocket = connectToRelay(dataPort_);
    if (relaySocket == INVALID_SOCKET) {
        logging::global().log(logging::Logger::Level::Warning, "TunnelAgent: no se pudo conectar data relay");
        return;
    }
    std::string code;
    {
        std::lock_guard guard(codeMutex_);
        code = code_;
    }
    if (!sendLine(relaySocket, "HOSTDATA code=" + code + " channel=" + channelId)) {
        closesocket(relaySocket);
        return;
    }
    auto ack = readLine(relaySocket);
    if (!ack || ack->rfind("OK", 0) != 0) {
        closesocket(relaySocket);
        return;
    }

    SOCKET localSocket = connectToLocal(localPort_.load(std::memory_order_acquire));
    if (localSocket == INVALID_SOCKET) {
        closesocket(relaySocket);
        return;
    }

    logging::global().log(logging::Logger::Level::Info, "TunnelAgent: canal " + channelId + " enlazado");

    auto pump = [](SOCKET from, SOCKET to) {
        std::array<char, 4096> buffer;
        while (true) {
            int received = recv(from, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received <= 0) {
                break;
            }
            int sentTotal = 0;
            while (sentTotal < received) {
                int sent = send(to, buffer.data() + sentTotal, received - sentTotal, 0);
                if (sent <= 0) {
                    return;
                }
                sentTotal += sent;
            }
        }
    };

    std::thread forwardThread([relaySocket, localSocket, pump]() {
        pump(relaySocket, localSocket);
        shutdown(localSocket, SD_SEND);
        shutdown(relaySocket, SD_RECEIVE);
    });

    pump(localSocket, relaySocket);
    shutdown(relaySocket, SD_SEND);
    shutdown(localSocket, SD_RECEIVE);

    forwardThread.join();
    closesocket(relaySocket);
    closesocket(localSocket);
    logging::global().log(logging::Logger::Level::Info, "TunnelAgent: canal " + channelId + " cerrado");
}

SOCKET TunnelAgent::connectToRelay(uint16_t port) const {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(relayHost_.c_str(), portStr.c_str(), &hints, &result) != 0) {
        return INVALID_SOCKET;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (sock == INVALID_SOCKET) {
            continue;
        }
        if (connect(sock, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == SOCKET_ERROR) {
            closesocket(sock);
            sock = INVALID_SOCKET;
            continue;
        }
        break;
    }
    freeaddrinfo(result);
    return sock;
}

SOCKET TunnelAgent::connectToLocal(uint16_t port) const {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return INVALID_SOCKET;
    }
    return sock;
}

} // namespace vic::transport
