#include "HostSession.h"

#include "FrameScaler.h"
#include "Logger.h"
#include "NvencEncoder.h"
#include "StreamConfig.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
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

vic::transport::TransportConfig buildTransportConfigFromEnv() {
    vic::transport::TransportConfig config{};

    const auto stunList = splitList(std::getenv("VIC_STUN_URLS"));
    if (stunList.empty()) {
        config.iceServers.push_back({"stun:stun.l.google.com:19302"});
    } else {
        for (const auto& url : stunList) {
            config.iceServers.push_back({url});
        }
    }

    // TURN server por defecto para conexiones WAN
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
    
    // Si no hay TURN configurado por env, usar servidor por defecto
    if (!turnConfigured) {
        vic::transport::IceServer defaultTurn{};
        defaultTurn.url = "turn:38.242.234.197:3478?transport=udp";
        defaultTurn.username = "vicuser";
        defaultTurn.credential = "vicpass2025";
        config.iceServers.push_back(defaultTurn);
        
        // También agregar TURN TCP como fallback
        vic::transport::IceServer turnTcp{};
        turnTcp.url = "turn:38.242.234.197:3478?transport=tcp";
        turnTcp.username = "vicuser";
        turnTcp.credential = "vicpass2025";
        config.iceServers.push_back(turnTcp);
        
        logging::global().log(logging::Logger::Level::Info, "Using default TURN server for WAN connectivity");
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
            if (const char* localPort = std::getenv("VIC_TUNNEL_LOCAL_PORT")) {
                if (*localPort) {
                    tunnel.localPort = static_cast<uint16_t>(std::strtoul(localPort, nullptr, 10));
                }
            }
            config.tunnel = tunnel;
        }
    }

    return config;
}

std::string generateCode() {
    static constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<size_t> dist(0, sizeof(alphabet) - 2);
    std::string result(6, '0');
    for (char& ch : result) {
        ch = alphabet[dist(rng)];
    }
    return result;
}

} // namespace

HostSession::HostSession()
        : capturer_(std::make_unique<vic::capture::DesktopCapturer>()),
          scaler_(std::make_unique<vic::capture::FrameScaler>()),
          encoder_(vic::encoder::createBestEncoder()),
          inputInjector_(std::make_unique<vic::input::InputInjector>()),
          transportServer_(std::make_unique<vic::transport::TransportServer>()) {
    // NO crear matchmakerClient_ aquí - se crea en signalingLoop con la URL correcta
    // Aplicar preset por defecto (Medium = 720p)
    streamConfig_.applyPreset(QualityPreset::Medium);
}

HostSession::~HostSession() {
    stop();
}

bool HostSession::start(uint16_t /*port*/) {
    if (running_.load()) {
        return true;
    }

    logging::global().log(logging::Logger::Level::Info, "HostSession: start requested");

    lastFrameTimestampMs_.store(0);
    // Solo resetear registered_ si no es registro externo
    if (!externalRegistration_) {
        registered_.store(false);
    }
    answerApplied_.store(false);

    logging::global().log(logging::Logger::Level::Info, "HostSession: inicializando capturador de escritorio...");
    if (!capturer_->initialize()) {
        logging::global().log(logging::Logger::Level::Error, "Failed to initialize desktop capturer");
        return false;
    }

    logging::global().log(logging::Logger::Level::Info, "HostSession: desktop capturer initialized");

    transportConfig_ = buildTransportConfigFromEnv();

    transportServer_->setInputHandlers(
        [this](const vic::input::MouseEvent& ev) {
            inputInjector_->inject(ev);
        },
        [this](const vic::input::KeyboardEvent& ev) {
            inputInjector_->inject(ev);
        });

    if (!transportServer_->start(transportConfig_)) {
        logging::global().log(logging::Logger::Level::Error, "Failed to start WebRTC transport server");
        return false;
    }

    logging::global().log(logging::Logger::Level::Info, "HostSession: transport server started");

    vic::transport::OfferBundle offerBundle;
    try {
        logging::global().log(logging::Logger::Level::Info, "HostSession: creating WebRTC offer bundle");
        offerBundle = transportServer_->createOfferBundle();
    } catch (const std::exception& ex) {
        logging::global().log(logging::Logger::Level::Error,
            std::string("Failed to create WebRTC offer: ") + ex.what());
        transportServer_->stop();
        return false;
    }

    logging::global().log(logging::Logger::Level::Info, "HostSession: WebRTC offer bundle ready");

    connectionInfo_ = vic::transport::ConnectionInfo{};
    // Usar código fijo si está configurado, sino generar uno aleatorio
    if (!fixedCode_.empty()) {
        connectionInfo_->code = fixedCode_;
        logging::global().log(logging::Logger::Level::Info,
            std::string("HostSession: using fixed session code ") + fixedCode_);
    } else {
        connectionInfo_->code = generateCode();
    }
    connectionInfo_->offer = std::move(offerBundle.description);
    connectionInfo_->iceCandidates = std::move(offerBundle.iceCandidates);
    connectionInfo_->iceServers = transportConfig_.iceServers;
    transportServer_->setConnectionInfo(*connectionInfo_);

    logging::global().log(logging::Logger::Level::Info,
        std::string("HostSession: provisional session code ") + connectionInfo_->code);

    running_.store(true);

    captureThread_ = std::thread(&HostSession::captureLoop, this);
    signalingThread_ = std::thread(&HostSession::signalingLoop, this);
    
    // Iniciar servidor TCP para conexiones LAN directas
    lanServerRunning_.store(true);
    lanServerThread_ = std::thread(&HostSession::lanServerLoop, this);

    logging::global().log(logging::Logger::Level::Info, "HostSession: threads started (including LAN server on port 9999)");
    return true;
}

void HostSession::stop() {
    running_.store(false);
    lanServerRunning_.store(false);
    
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    if (signalingThread_.joinable()) {
        signalingThread_.join();
    }
    if (lanServerThread_.joinable()) {
        lanServerThread_.join();
    }
    if (transportServer_) {
        transportServer_->stop();
    }
    connectionInfo_.reset();

    logging::global().log(logging::Logger::Level::Info, "HostSession: stopped");
}

void HostSession::captureLoop() {
    logging::global().log(logging::Logger::Level::Info, 
        "Host capture loop started - Quality: " + 
        std::to_string(streamConfig_.maxWidth) + "x" + std::to_string(streamConfig_.maxHeight) +
        " @ " + std::to_string(streamConfig_.targetBitrateKbps) + " kbps");
    
    uint32_t encoderWidth = 0;
    uint32_t encoderHeight = 0;
    using namespace std::chrono_literals;
    
    // Para cálculo de FPS
    auto lastFpsUpdate = std::chrono::steady_clock::now();
    uint64_t framesThisSecond = 0;
    uint64_t bytesThisSecond = 0;

    while (running_.load()) {
        if (!answerApplied_.load()) {
            std::this_thread::sleep_for(10ms);
            continue;
        }

        auto frame = capturer_->captureFrame();
        if (!frame) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        // ========== ESCALADO OPCIONAL ==========
        // Si streamConfig indica resolución menor, escalar
        vic::capture::DesktopFrame* frameToEncode = frame.get();
        std::unique_ptr<vic::capture::DesktopFrame> scaledFrame;
        
        if (frame->width > streamConfig_.maxWidth || frame->height > streamConfig_.maxHeight) {
            scaledFrame = scaler_->scale(*frame, streamConfig_.maxWidth, streamConfig_.maxHeight);
            if (scaledFrame) {
                frameToEncode = scaledFrame.get();
            }
        }

        // Overlay de cursor (en el frame escalado)
        CURSORINFO ci{ sizeof(CURSORINFO) };
        if (streamConfig_.enableCursorOverlay && GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor) {
            ICONINFO ii{};
            if (GetIconInfo(ci.hCursor, &ii)) {
                const int hotspotX = static_cast<int>(ii.xHotspot);
                const int hotspotY = static_cast<int>(ii.yHotspot);
                if (ii.hbmMask) DeleteObject(ii.hbmMask);
                if (ii.hbmColor) DeleteObject(ii.hbmColor);
                
                // Calcular posición del cursor escalada
                float scaleX = static_cast<float>(frameToEncode->width) / frame->width;
                float scaleY = static_cast<float>(frameToEncode->height) / frame->height;
                const int cx = static_cast<int>((ci.ptScreenPos.x - hotspotX) * scaleX);
                const int cy = static_cast<int>((ci.ptScreenPos.y - hotspotY) * scaleY);
                
                // Dibujar cursor invertido (5x5 pixels)
                for (int oy = 0; oy < 5; ++oy) {
                    const int py = cy + oy;
                    if (py < 0 || py >= static_cast<int>(frameToEncode->height)) continue;
                    for (int ox = 0; ox < 5; ++ox) {
                        const int px = cx + ox;
                        if (px < 0 || px >= static_cast<int>(frameToEncode->width)) continue;
                        const size_t idx = (static_cast<size_t>(py) * frameToEncode->width + px) * 4;
                        frameToEncode->bgraData[idx + 0] = 255 - frameToEncode->bgraData[idx + 0];
                        frameToEncode->bgraData[idx + 1] = 255 - frameToEncode->bgraData[idx + 1];
                        frameToEncode->bgraData[idx + 2] = 255 - frameToEncode->bgraData[idx + 2];
                    }
                }
            }
        }

        // Configurar encoder si cambió la resolución
        if (frameToEncode->width != encoderWidth || frameToEncode->height != encoderHeight) {
            encoderWidth = frameToEncode->width;
            encoderHeight = frameToEncode->height;
            if (!encoder_->Configure(encoderWidth, encoderHeight, streamConfig_.targetBitrateKbps)) {
                logging::global().log(logging::Logger::Level::Warning, "Failed to configure encoder");
                continue;
            }
            logging::global().log(logging::Logger::Level::Info,
                "[Host] Encoder configurado: " + std::to_string(encoderWidth) + "x" + 
                std::to_string(encoderHeight) + " @ " + std::to_string(streamConfig_.targetBitrateKbps) + " kbps");
        }

        // Verificar si el DataChannel acaba de abrirse y necesita keyframe
        if (transportServer_->needsInitialKeyframe()) {
            logging::global().log(logging::Logger::Level::Info, 
                "[Host] DataChannel abierto - forzando keyframe inicial");
            encoder_->forceNextKeyframe();
        }

        auto encodedOpt = encoder_->EncodeFrame(*frameToEncode);
        if (!encodedOpt) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        // *** IMPORTANTE: Guardar resolución ORIGINAL para que el viewer calcule coordenadas correctas ***
        encodedOpt->originalWidth = frame->width;
        encodedOpt->originalHeight = frame->height;

        if (!transportServer_->sendFrame(*encodedOpt)) {
            std::this_thread::sleep_for(5ms);
            continue;
        }

        // ========== MÉTRICAS ==========
        framesThisSecond++;
        bytesThisSecond += encodedOpt->payload.size();
        frameCount_.fetch_add(1, std::memory_order_relaxed);
        bytesSent_.fetch_add(encodedOpt->payload.size(), std::memory_order_relaxed);

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFpsUpdate).count();
        if (elapsed >= 1000) {
            currentFps_.store(static_cast<uint32_t>(framesThisSecond), std::memory_order_relaxed);
            currentBitrateKbps_.store(static_cast<uint32_t>((bytesThisSecond * 8) / 1000), std::memory_order_relaxed);
            
            // Log periódico de rendimiento
            logging::global().log(logging::Logger::Level::Debug,
                "[Host] FPS=" + std::to_string(framesThisSecond) + 
                " Bitrate=" + std::to_string((bytesThisSecond * 8) / 1000) + "kbps" +
                " Resolution=" + std::to_string(encoderWidth) + "x" + std::to_string(encoderHeight));
            
            framesThisSecond = 0;
            bytesThisSecond = 0;
            lastFpsUpdate = now;
        }

        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now.time_since_epoch()).count();
        lastFrameTimestampMs_.store(static_cast<uint64_t>(nowMs));

        // Control de framerate: Limitar a maxFramerate si está configurado
        if (streamConfig_.maxFramerate > 0 && streamConfig_.maxFramerate < 60) {
            auto targetFrameTime = std::chrono::milliseconds(1000 / streamConfig_.maxFramerate);
            std::this_thread::sleep_for(targetFrameTime / 2);  // Sleep parcial, DXGI hace el resto
        }
    }

    logging::global().log(logging::Logger::Level::Info, "Host capture loop stopped");
}

void HostSession::signalingLoop() {
    using namespace std::chrono_literals;
    logging::global().log(logging::Logger::Level::Info, "[Host] signalingLoop iniciado");
    
    while (running_.load()) {
        if (!connectionInfo_) {
            logging::global().log(logging::Logger::Level::Debug, "[Host] signalingLoop: esperando connectionInfo...");
            std::this_thread::sleep_for(500ms);
            continue;
        }

        if (!matchmakerClient_) {
            logging::global().log(logging::Logger::Level::Info, 
                "[Host] Creando matchmakerClient con URL: " + std::string(matchmakerUrl_.begin(), matchmakerUrl_.end()));
            matchmakerClient_ = std::make_unique<vic::matchmaking::MatchmakerClient>(matchmakerUrl_);
        }

        // Log del estado actual
        logging::global().log(logging::Logger::Level::Debug, 
            "[Host] Estado: registered=" + std::string(registered_.load() ? "true" : "false") + 
            ", answerApplied=" + std::string(answerApplied_.load() ? "true" : "false"));

        if (!registered_.load()) {
            auto result = matchmakerClient_->registerHost(*connectionInfo_);
            if (result) {
                if (*result != connectionInfo_->code) {
                    connectionInfo_->code = *result;
                }
                transportServer_->setConnectionInfo(*connectionInfo_);
                registered_.store(true);
                logging::global().log(logging::Logger::Level::Info, "Host registrado en matchmaker con código " + connectionInfo_->code);
            } else {
                logging::global().log(logging::Logger::Level::Warning, "Registro matchmaker falló, reintento...");
                std::this_thread::sleep_for(retryInterval_);
            }
            continue;
        }

        if (!answerApplied_.load()) {
            logging::global().log(logging::Logger::Level::Info, "[Host] Buscando answer para código: " + connectionInfo_->code);
            auto answerBundle = matchmakerClient_->fetchViewerAnswer(connectionInfo_->code);
            if (!answerBundle) {
                logging::global().log(logging::Logger::Level::Debug, "[Host] No hay answer aun, reintentando en 3s...");
                std::this_thread::sleep_for(retryInterval_);
                continue;
            }

            logging::global().log(logging::Logger::Level::Info, "[Host] Answer recibido! Aplicando...");
            if (!transportServer_->applyAnswer(answerBundle->description)) {
                logging::global().log(logging::Logger::Level::Warning, "Aplicar respuesta WebRTC falló, reintento...");
                std::this_thread::sleep_for(retryInterval_);
                continue;
            }

            logging::global().log(logging::Logger::Level::Info, "[Host] Agregando " + std::to_string(answerBundle->iceCandidates.size()) + " candidatos remotos");
            for (const auto& candidate : answerBundle->iceCandidates) {
                logging::global().log(logging::Logger::Level::Debug, "[Host] Candidato: " + candidate.candidate);
                transportServer_->addRemoteCandidate(candidate);
            }

            answerApplied_.store(true);
            logging::global().log(logging::Logger::Level::Info, "Respuesta del viewer aplicada, streaming habilitado");
            continue;
        }

        std::this_thread::sleep_for(1s);
    }
}

// =========== LAN SERVER LOOP ===========
void HostSession::lanServerLoop() {
    logging::global().log(logging::Logger::Level::Info, "[LAN] Iniciando servidor TCP en puerto " + std::to_string(LAN_PORT));
    
    // Inicializar WinSock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        logging::global().log(logging::Logger::Level::Error, "[LAN] WSAStartup falló");
        return;
    }
    
    // Crear socket servidor
    SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) {
        logging::global().log(logging::Logger::Level::Error, "[LAN] No se pudo crear socket servidor");
        WSACleanup();
        return;
    }
    
    // Permitir reutilización de dirección
    int opt = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    
    // Configurar como no-bloqueante para poder salir limpiamente
    u_long mode = 1;
    ioctlsocket(serverSocket, FIONBIO, &mode);
    
    // Bind al puerto
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(LAN_PORT);
    
    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        logging::global().log(logging::Logger::Level::Error, "[LAN] Bind falló en puerto " + std::to_string(LAN_PORT));
        closesocket(serverSocket);
        WSACleanup();
        return;
    }
    
    if (listen(serverSocket, 5) == SOCKET_ERROR) {
        logging::global().log(logging::Logger::Level::Error, "[LAN] Listen falló");
        closesocket(serverSocket);
        WSACleanup();
        return;
    }
    
    logging::global().log(logging::Logger::Level::Info, "[LAN] Servidor TCP escuchando en puerto " + std::to_string(LAN_PORT));
    
    while (lanServerRunning_.load() && running_.load()) {
        // Esperar conexión con timeout corto para poder verificar lanServerRunning_
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(serverSocket, &readSet);
        
        timeval timeout{0, 500000}; // 500ms
        int selectResult = select(0, &readSet, nullptr, nullptr, &timeout);
        
        if (selectResult <= 0) {
            continue; // Timeout o error, verificar si debemos seguir
        }
        
        // Aceptar conexión
        sockaddr_in clientAddr{};
        int clientAddrLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(serverSocket, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        
        if (clientSocket == INVALID_SOCKET) {
            continue;
        }
        
        char clientIP[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
        logging::global().log(logging::Logger::Level::Info, 
            "[LAN] Nueva conexión desde " + std::string(clientIP));
        
        // Verificar que tenemos connectionInfo_ disponible
        if (!connectionInfo_.has_value()) {
            logging::global().log(logging::Logger::Level::Warning, "[LAN] No hay connectionInfo disponible");
            closesocket(clientSocket);
            continue;
        }
        
        try {
            // Configurar socket cliente como bloqueante con timeout
            mode = 0;
            ioctlsocket(clientSocket, FIONBIO, &mode);
            DWORD timeout_ms = 10000; // 10 segundos
            setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            
            // Construir JSON con oferta SDP e ICE candidates
            std::string offer = "{\"sdp\":\"";
            for (char c : connectionInfo_->offer.sdp) {
                if (c == '\n') offer += "\\n";
                else if (c == '\r') offer += "\\r";
                else if (c == '\\') offer += "\\\\";
                else if (c == '"') offer += "\\\"";
                else offer += c;
            }
            offer += "\",\"type\":\"offer\",\"ice\":[";
            
            bool firstCandidate = true;
            for (const auto& cand : connectionInfo_->iceCandidates) {
                if (!firstCandidate) offer += ",";
                firstCandidate = false;
                offer += "{\"candidate\":\"" + cand.candidate + "\",\"sdpMid\":\"" + cand.sdpMid + 
                         "\",\"sdpMLineIndex\":" + std::to_string(cand.sdpMLineIndex) + "}";
            }
            offer += "]}";
            
            // Enviar tamaño y datos
            uint32_t offerSize = htonl(static_cast<uint32_t>(offer.size()));
            send(clientSocket, reinterpret_cast<const char*>(&offerSize), sizeof(offerSize), 0);
            send(clientSocket, offer.c_str(), static_cast<int>(offer.size()), 0);
            
            logging::global().log(logging::Logger::Level::Info, 
                "[LAN] Oferta enviada (" + std::to_string(offer.size()) + " bytes), esperando respuesta...");
            
            // Recibir respuesta
            uint32_t answerSize = 0;
            int bytesRead = recv(clientSocket, reinterpret_cast<char*>(&answerSize), sizeof(answerSize), 0);
            if (bytesRead != sizeof(answerSize)) {
                throw std::runtime_error("No se pudo leer tamaño de respuesta");
            }
            answerSize = ntohl(answerSize);
            
            if (answerSize > 1024 * 1024) {
                throw std::runtime_error("Respuesta demasiado grande");
            }
            
            std::string answerData(answerSize, '\0');
            size_t totalRead = 0;
            while (totalRead < answerSize) {
                int chunk = recv(clientSocket, answerData.data() + totalRead, 
                    static_cast<int>(answerSize - totalRead), 0);
                if (chunk <= 0) {
                    throw std::runtime_error("Conexión cerrada mientras se recibía respuesta");
                }
                totalRead += chunk;
            }
            
            logging::global().log(logging::Logger::Level::Info, 
                "[LAN] Respuesta recibida (" + std::to_string(answerSize) + " bytes)");
            
            // Parsear respuesta JSON
            vic::transport::OfferBundle answerBundle{};
            
            // Extraer SDP
            size_t sdpStart = answerData.find("\"sdp\":\"");
            if (sdpStart != std::string::npos) {
                sdpStart += 7;
                size_t sdpEnd = answerData.find("\",", sdpStart);
                if (sdpEnd != std::string::npos) {
                    std::string sdp = answerData.substr(sdpStart, sdpEnd - sdpStart);
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
                    answerBundle.description.sdp = decodedSdp;
                    answerBundle.description.type = "answer";
                }
            }
            
            // Extraer ICE candidates
            size_t iceStart = answerData.find("\"ice\":[");
            if (iceStart != std::string::npos) {
                size_t iceEnd = answerData.find("]", iceStart);
                if (iceEnd != std::string::npos) {
                    std::string iceSection = answerData.substr(iceStart + 7, iceEnd - iceStart - 7);
                    size_t pos = 0;
                    while ((pos = iceSection.find("{\"candidate\":\"", pos)) != std::string::npos) {
                        pos += 14;
                        size_t candEnd = iceSection.find("\"", pos);
                        if (candEnd != std::string::npos) {
                            vic::transport::IceCandidate cand{};
                            cand.candidate = iceSection.substr(pos, candEnd - pos);
                            cand.sdpMid = "0";
                            cand.sdpMLineIndex = 0;
                            answerBundle.iceCandidates.push_back(cand);
                        }
                    }
                }
            }
            
            if (answerBundle.description.sdp.empty()) {
                throw std::runtime_error("No se pudo extraer SDP de la respuesta");
            }
            
            logging::global().log(logging::Logger::Level::Info, 
                "[LAN] Respuesta parseada, aplicando al transport...");
            
            // Aplicar respuesta al transport
            if (!transportServer_->applyAnswer(answerBundle.description)) {
                throw std::runtime_error("No se pudo aplicar respuesta WebRTC");
            }
            
            for (const auto& candidate : answerBundle.iceCandidates) {
                transportServer_->addRemoteCandidate(candidate);
            }
            
            answerApplied_.store(true);
            logging::global().log(logging::Logger::Level::Info, 
                "[LAN] Conexión LAN establecida con " + std::string(clientIP));
            
        } catch (const std::exception& ex) {
            logging::global().log(logging::Logger::Level::Error,
                "[LAN] Error procesando conexión: " + std::string(ex.what()));
        }
        
        closesocket(clientSocket);
    }
    
    closesocket(serverSocket);
    WSACleanup();
    logging::global().log(logging::Logger::Level::Info, "[LAN] Servidor TCP detenido");
}

} // namespace vic::pipeline
