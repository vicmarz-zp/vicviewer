#include "ViewerSession.h"

#include "Logger.h"

#include <cstdlib>
#include <sstream>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "Ws2_32.lib")

namespace vic::pipeline {

namespace {

std::vector<std::string> splitList(const char* value) {
    std::vector<std::string> entries;
    if (!value || *value == '\0') {
        return entries;
    }
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ';')) {
        if (item.empty()) {
            continue;
        }
        entries.push_back(item);
    }
    return entries;
}

vic::transport::TransportConfig buildViewerConfig(const std::vector<vic::transport::IceServer>& hostServers) {
    vic::transport::TransportConfig config{};
    config.iceServers = hostServers;

    const auto extraStun = splitList(std::getenv("VIC_STUN_URLS"));
    for (const auto& stun : extraStun) {
        config.iceServers.push_back({stun});
    }

    bool turnConfigured = false;
    if (const char* turnUrl = std::getenv("VIC_TURN_URL")) {
        if (*turnUrl) {
            vic::transport::IceServer server{};
            server.url = turnUrl;
            if (const char* user = std::getenv("VIC_TURN_USERNAME")) {
                if (*user) {
                    server.username = user;
                }
            }
            if (const char* pass = std::getenv("VIC_TURN_PASSWORD")) {
                if (*pass) {
                    server.credential = pass;
                }
            }
            if (const char* relay = std::getenv("VIC_TURN_TRANSPORT")) {
                if (*relay) {
                    server.relayTransport = relay;
                }
            }
            config.iceServers.push_back(std::move(server));
            turnConfigured = true;
        }
    }
    
    // Si no hay TURN configurado, agregar servidor por defecto para WAN
    if (!turnConfigured) {
        vic::transport::IceServer defaultTurn{};
        defaultTurn.url = "turn:38.242.234.197:3478?transport=udp";
        defaultTurn.username = "vicuser";
        defaultTurn.credential = "vicpass2025";
        config.iceServers.push_back(defaultTurn);
        
        vic::transport::IceServer turnTcp{};
        turnTcp.url = "turn:38.242.234.197:3478?transport=tcp";
        turnTcp.username = "vicuser";
        turnTcp.credential = "vicpass2025";
        config.iceServers.push_back(turnTcp);
    }

    if (config.iceServers.empty()) {
        config.iceServers.push_back({"stun:stun.l.google.com:19302"});
    }

    if (const char* tunnelHost = std::getenv("VIC_TUNNEL_HOST")) {
        if (*tunnelHost) {
            vic::transport::TunnelConfig tunnel{};
            tunnel.relayHost = tunnelHost;
            if (const char* controlPort = std::getenv("VIC_TUNNEL_CONTROL_PORT")) {
                if (*controlPort) {
                    tunnel.controlPort = static_cast<uint16_t>(std::strtoul(controlPort, nullptr, 10));
                }
            }
            if (const char* dataPort = std::getenv("VIC_TUNNEL_DATA_PORT")) {
                if (*dataPort) {
                    tunnel.dataPort = static_cast<uint16_t>(std::strtoul(dataPort, nullptr, 10));
                }
            }
            config.tunnel = tunnel;
        }
    }

    return config;
}

} // namespace

ViewerSession::ViewerSession()
        : client_(std::make_unique<vic::transport::TransportClient>()),
          decoder_(vic::decoder::createVp8Decoder()) {
    matchmakerClient_ = std::make_unique<vic::matchmaking::MatchmakerClient>(vic::matchmaking::MatchmakerClient::kDefaultServiceUrl);
}

ViewerSession::~ViewerSession() {
    disconnect();
}

bool ViewerSession::connect(const std::string& code) {
    if (connected_.load()) {
        return true;
    }

    if (!matchmakerClient_) {
        matchmakerClient_ = std::make_unique<vic::matchmaking::MatchmakerClient>(vic::matchmaking::MatchmakerClient::kDefaultServiceUrl);
    }

    if (code.empty()) {
        logging::global().log(logging::Logger::Level::Error, "ViewerSession requiere un código del matchmaker");
        return false;
    }

    try {
        auto connectionInfo = matchmakerClient_->resolveCode(code);
        if (!connectionInfo) {
            logging::global().log(logging::Logger::Level::Error, "No se pudo resolver código " + code);
            return false;
        }

        logging::global().log(logging::Logger::Level::Info, "[ViewerSession] Construyendo config de transport...");
        clientTransportConfig_ = buildViewerConfig(connectionInfo->iceServers);
        logging::global().log(logging::Logger::Level::Info, "[ViewerSession] setConnectionInfo...");
        client_->setConnectionInfo(*connectionInfo);

        logging::global().log(logging::Logger::Level::Info, "[ViewerSession] Iniciando WebRTC (client_->start)...");
        if (!client_->start(clientTransportConfig_)) {
            logging::global().log(logging::Logger::Level::Error, "No se pudo iniciar WebRTC en viewer");
            return false;
        }
        logging::global().log(logging::Logger::Level::Info, "[ViewerSession] WebRTC iniciado OK");

        client_->setFrameHandler([this](const vic::encoder::EncodedFrame& frame) {
            if (decoder_) {
                auto decoded = decoder_->decode(frame);
                if (decoded && frameCallback_) {
                    frameCallback_(*decoded);
                }
            }
        });

        if (connectionInfo->offer.sdp.find("a=ice-ufrag:") == std::string::npos) {
            logging::global().log(logging::Logger::Level::Warning,
                "Matchmaker devolvió oferta sin credenciales ICE:\n" + connectionInfo->offer.sdp);
        }

        // acceptOffer puede lanzar si la oferta es inválida; manejar para no colgar
        logging::global().log(logging::Logger::Level::Info, "[ViewerSession] Aceptando oferta WebRTC...");
        auto answerBundle = client_->acceptOffer(connectionInfo->offer);
        logging::global().log(logging::Logger::Level::Info, "[ViewerSession] Oferta aceptada, agregando candidatos remotos...");
        for (const auto& candidate : connectionInfo->iceCandidates) {
            logging::global().log(logging::Logger::Level::Debug, "[ViewerSession] Agregando candidato: " + candidate.candidate);
            client_->addRemoteCandidate(candidate);
        }
        logging::global().log(logging::Logger::Level::Info, "[ViewerSession] Enviando respuesta al matchmaker...");
        if (!matchmakerClient_->submitViewerAnswer(connectionInfo->code, answerBundle)) {
            logging::global().log(logging::Logger::Level::Warning,
                "Envío de respuesta WebRTC al matchmaker falló");
        } else {
            logging::global().log(logging::Logger::Level::Info, "[ViewerSession] Respuesta enviada OK, esperando conexión...");
        }

        connected_.store(true);
        lastCode_ = connectionInfo->code;
        logging::global().log(logging::Logger::Level::Info, "[ViewerSession] Viewer marcado como conectado, esperando frames via DataChannel");
        return true;
    } catch (const std::exception& ex) {
        logging::global().log(logging::Logger::Level::Error,
            std::string("ViewerSession::connect falló: ") + ex.what());
        // Asegurar estado consistente
        try { client_->stop(); } catch (...) {}
        connected_.store(false);
        return false;
    }
}

// =========== LAN DIRECT CONNECTION ===========
bool ViewerSession::connectDirect(const std::string& hostIp, uint16_t port) {
    if (connected_.load()) {
        return true;
    }
    
    logging::global().log(logging::Logger::Level::Info, 
        "[ViewerSession] Iniciando conexión LAN directa a " + hostIp + ":" + std::to_string(port));
    
    // Inicializar WinSock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        logging::global().log(logging::Logger::Level::Error, "[ViewerSession] WSAStartup falló");
        return false;
    }
    
    // Crear socket TCP
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        logging::global().log(logging::Logger::Level::Error, "[ViewerSession] No se pudo crear socket TCP");
        WSACleanup();
        return false;
    }
    
    // Configurar timeout de conexión (5 segundos)
    DWORD timeout = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    
    // Conectar al host
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, hostIp.c_str(), &serverAddr.sin_addr);
    
    if (::connect(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        logging::global().log(logging::Logger::Level::Error, 
            "[ViewerSession] No se pudo conectar a " + hostIp + ":" + std::to_string(port));
        closesocket(sock);
        WSACleanup();
        return false;
    }
    
    logging::global().log(logging::Logger::Level::Info, "[ViewerSession] Conexión TCP establecida");
    
    try {
        // Recibir tamaño del paquete de conexión (4 bytes)
        uint32_t packetSize = 0;
        int bytesRead = recv(sock, reinterpret_cast<char*>(&packetSize), sizeof(packetSize), 0);
        if (bytesRead != sizeof(packetSize)) {
            throw std::runtime_error("No se pudo leer tamaño del paquete");
        }
        packetSize = ntohl(packetSize);
        
        if (packetSize > 1024 * 1024) { // Max 1MB
            throw std::runtime_error("Paquete demasiado grande");
        }
        
        // Recibir datos de conexión (JSON con SDP offer y ICE candidates)
        std::string connectionData(packetSize, '\0');
        size_t totalRead = 0;
        while (totalRead < packetSize) {
            int chunk = recv(sock, connectionData.data() + totalRead, 
                static_cast<int>(packetSize - totalRead), 0);
            if (chunk <= 0) {
                throw std::runtime_error("Conexión cerrada mientras se recibían datos");
            }
            totalRead += chunk;
        }
        
        logging::global().log(logging::Logger::Level::Info, 
            "[ViewerSession] Recibido paquete de conexión (" + std::to_string(packetSize) + " bytes)");
        
        // Parsear JSON manualmente (formato simple)
        // Formato: {"sdp":"...","type":"offer","ice":[{"candidate":"...","sdpMid":"...","sdpMLineIndex":0},...]}
        vic::transport::ConnectionInfo connInfo{};
        connInfo.code = "LAN";
        
        // Extraer SDP
        size_t sdpStart = connectionData.find("\"sdp\":\"");
        if (sdpStart != std::string::npos) {
            sdpStart += 7;
            size_t sdpEnd = connectionData.find("\",", sdpStart);
            if (sdpEnd != std::string::npos) {
                std::string sdp = connectionData.substr(sdpStart, sdpEnd - sdpStart);
                // Decodificar escapes
                std::string decodedSdp;
                for (size_t i = 0; i < sdp.length(); i++) {
                    if (sdp[i] == '\\' && i + 1 < sdp.length()) {
                        if (sdp[i+1] == 'n') { decodedSdp += '\n'; i++; }
                        else if (sdp[i+1] == 'r') { decodedSdp += '\r'; i++; }
                        else if (sdp[i+1] == '\\') { decodedSdp += '\\'; i++; }
                        else if (sdp[i+1] == '"') { decodedSdp += '"'; i++; }
                        else decodedSdp += sdp[i];
                    } else {
                        decodedSdp += sdp[i];
                    }
                }
                connInfo.offer.sdp = decodedSdp;
                connInfo.offer.type = "offer";
            }
        }
        
        // Extraer ICE candidates
        size_t iceStart = connectionData.find("\"ice\":[");
        if (iceStart != std::string::npos) {
            size_t iceEnd = connectionData.find("]", iceStart);
            if (iceEnd != std::string::npos) {
                std::string iceSection = connectionData.substr(iceStart + 7, iceEnd - iceStart - 7);
                // Parsear cada candidato
                size_t pos = 0;
                while ((pos = iceSection.find("{\"candidate\":\"", pos)) != std::string::npos) {
                    pos += 14;
                    size_t candEnd = iceSection.find("\"", pos);
                    if (candEnd != std::string::npos) {
                        vic::transport::IceCandidate cand{};
                        cand.candidate = iceSection.substr(pos, candEnd - pos);
                        cand.sdpMid = "0";
                        cand.sdpMLineIndex = 0;
                        connInfo.iceCandidates.push_back(cand);
                    }
                }
            }
        }
        
        if (connInfo.offer.sdp.empty()) {
            throw std::runtime_error("No se pudo extraer SDP de la oferta");
        }
        
        logging::global().log(logging::Logger::Level::Info, 
            "[ViewerSession] Oferta parseada, " + std::to_string(connInfo.iceCandidates.size()) + " candidatos ICE");
        
        // Configurar transport para LAN (solo STUN local, sin TURN)
        vic::transport::TransportConfig lanConfig{};
        lanConfig.iceServers.push_back({"stun:stun.l.google.com:19302"});
        
        clientTransportConfig_ = lanConfig;
        client_->setConnectionInfo(connInfo);
        
        if (!client_->start(clientTransportConfig_)) {
            throw std::runtime_error("No se pudo iniciar WebRTC");
        }
        
        client_->setFrameHandler([this](const vic::encoder::EncodedFrame& frame) {
            if (decoder_) {
                auto decoded = decoder_->decode(frame);
                if (decoded && frameCallback_) {
                    frameCallback_(*decoded);
                }
            }
        });
        
        // Generar respuesta
        logging::global().log(logging::Logger::Level::Info, "[ViewerSession] Aceptando oferta y generando respuesta...");
        auto answerBundle = client_->acceptOffer(connInfo.offer);
        
        for (const auto& candidate : connInfo.iceCandidates) {
            client_->addRemoteCandidate(candidate);
        }
        
        // Construir respuesta JSON
        std::string answer = "{\"sdp\":\"";
        for (char c : answerBundle.answer.sdp) {
            if (c == '\n') answer += "\\n";
            else if (c == '\r') answer += "\\r";
            else if (c == '\\') answer += "\\\\";
            else if (c == '"') answer += "\\\"";
            else answer += c;
        }
        answer += "\",\"type\":\"answer\",\"ice\":[";
        
        bool firstCandidate = true;
        for (const auto& cand : answerBundle.iceCandidates) {
            if (!firstCandidate) answer += ",";
            firstCandidate = false;
            answer += "{\"candidate\":\"" + cand.candidate + "\",\"sdpMid\":\"" + cand.sdpMid + 
                     "\",\"sdpMLineIndex\":" + std::to_string(cand.sdpMLineIndex) + "}";
        }
        answer += "]}";
        
        // Enviar respuesta
        uint32_t answerSize = htonl(static_cast<uint32_t>(answer.size()));
        send(sock, reinterpret_cast<const char*>(&answerSize), sizeof(answerSize), 0);
        send(sock, answer.c_str(), static_cast<int>(answer.size()), 0);
        
        logging::global().log(logging::Logger::Level::Info, "[ViewerSession] Respuesta enviada al host");
        
        closesocket(sock);
        WSACleanup();
        
        connected_.store(true);
        logging::global().log(logging::Logger::Level::Info, "[ViewerSession] Conexión LAN establecida, esperando frames");
        return true;
        
    } catch (const std::exception& ex) {
        logging::global().log(logging::Logger::Level::Error,
            std::string("[ViewerSession] connectDirect falló: ") + ex.what());
        closesocket(sock);
        WSACleanup();
        try { client_->stop(); } catch (...) {}
        connected_.store(false);
        return false;
    }
}

void ViewerSession::disconnect() {
    if (!connected_.load()) {
        return;
    }
    client_->stop();
    connected_.store(false);
    reconnectRunning_.store(false);
    if (reconnectThread_.joinable()) {
        reconnectThread_.join();
    }
}

void ViewerSession::setFrameCallback(std::function<void(const vic::capture::DesktopFrame&)> callback) {
    frameCallback_ = std::move(callback);
}

bool ViewerSession::sendMouseEvent(const vic::input::MouseEvent& ev) {
    return client_->sendMouseEvent(ev);
}

bool ViewerSession::sendKeyboardEvent(const vic::input::KeyboardEvent& ev) {
    return client_->sendKeyboardEvent(ev);
}

void ViewerSession::enableAutoReconnect(const std::string& code) {
    lastCode_ = code;
    reconnectRunning_.store(true);
    if (!matchmakerClient_) {
        matchmakerClient_ = std::make_unique<vic::matchmaking::MatchmakerClient>(vic::matchmaking::MatchmakerClient::kDefaultServiceUrl);
    }
    if (reconnectThread_.joinable()) {
        reconnectThread_.join();
    }
    reconnectThread_ = std::thread([this]() {
        while (reconnectRunning_.load()) {
            std::this_thread::sleep_for(resolveInterval_);
            if (connected_.load() || lastCode_.empty()) {
                continue;
            }
            if (!matchmakerClient_) {
                continue;
            }
            try {
                auto info = matchmakerClient_->resolveCode(lastCode_);
                if (!info) {
                    continue;
                }
                client_->stop();
                clientTransportConfig_ = buildViewerConfig(info->iceServers);
                client_->setConnectionInfo(*info);
                if (!client_->start(clientTransportConfig_)) {
                    continue;
                }
                client_->setFrameHandler([this](const vic::encoder::EncodedFrame& frame) {
                    if (decoder_) {
                        auto decoded = decoder_->decode(frame);
                        if (decoded && frameCallback_) {
                            frameCallback_(*decoded);
                        }
                    }
                });
                if (info->offer.sdp.find("a=ice-ufrag:") == std::string::npos) {
                    logging::global().log(logging::Logger::Level::Warning,
                        "Matchmaker devolvió oferta sin credenciales ICE (auto-reconnect):\n" + info->offer.sdp);
                }

                auto answerBundle = client_->acceptOffer(info->offer);
                for (const auto& candidate : info->iceCandidates) {
                    client_->addRemoteCandidate(candidate);
                }
                matchmakerClient_->submitViewerAnswer(info->code, answerBundle);
                connected_.store(true);
                logging::global().log(logging::Logger::Level::Info, "Viewer reconectado via matchmaker");
            } catch (const std::exception& ex) {
                logging::global().log(logging::Logger::Level::Warning,
                    std::string("Auto-reconnect falló: ") + ex.what());
                try { client_->stop(); } catch (...) {}
                connected_.store(false);
            }
        }
    });
}

void ViewerSession::disableAutoReconnect() {
    reconnectRunning_.store(false);
    if (reconnectThread_.joinable()) {
        reconnectThread_.join();
    }
}

} // namespace vic::pipeline
