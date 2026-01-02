#pragma once

#include "Transport.h"

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <mutex>

namespace vic::transport {

class TunnelAgent {
public:
    TunnelAgent(std::string relayHost,
        uint16_t controlPort,
        uint16_t dataPort);
    ~TunnelAgent();

    bool start(const ConnectionInfo& info, uint16_t localPort);
    void stop();

    void updateConnection(const ConnectionInfo& info);

private:
    void controlLoop();
    void handleControlMessage(const std::string& line);
    void launchBridge(const std::string& channelId);
    void bridgeLoop(const std::string& channelId);

    SOCKET connectToRelay(uint16_t port) const;
    SOCKET connectToLocal(uint16_t port) const;

    std::atomic_bool running_{false};
    std::atomic_bool controlConnected_{false};
    std::atomic_bool stopRequested_{false};

    std::string relayHost_;
    uint16_t controlPort_{};
    uint16_t dataPort_{};

    std::string code_;
    std::atomic<uint16_t> localPort_{0};

    mutable std::mutex codeMutex_;

    std::thread controlThread_;
};

} // namespace vic::transport
