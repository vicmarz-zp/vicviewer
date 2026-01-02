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

    logging::global().log(logging::Logger::Level::Info, "HostSession: threads started");
    return true;
}

void HostSession::stop() {
    running_.store(false);
    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    if (signalingThread_.joinable()) {
        signalingThread_.join();
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

} // namespace vic::pipeline
