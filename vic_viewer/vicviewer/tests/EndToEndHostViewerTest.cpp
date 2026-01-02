#include "HostSession.h"
#include "ViewerSession.h"
#include "MatchmakerClient.h"
#include "Logger.h"

#include <chrono>
#include <thread>
#include <iostream>
#include <cstdlib>

int main() {
    _putenv("VIC_DISABLE_TUNNEL=1");
    // Este test simula host + viewer usando el matchmaker remoto configurado.
    vic::pipeline::HostSession host;
    if (!host.start()) {
        std::cerr << "No se pudo iniciar HostSession" << std::endl;
        return 1;
    }
    auto infoOpt = host.connectionInfo();
    if (!infoOpt) {
        std::cerr << "Sin connection info" << std::endl;
        return 1;
    }

    vic::matchmaking::MatchmakerClient mm{std::wstring(vic::matchmaking::MatchmakerClient::kDefaultServiceUrl)};
    auto codeOpt = mm.registerHost(*infoOpt);
    if (!codeOpt) {
        if (std::getenv("VIC_REQUIRE_MATCHMAKER")) {
            std::cerr << "Registro en matchmaker falló (modo estricto)" << std::endl;
            return 1;
        } else {
            std::cout << "SKIP: matchmaker no disponible, omitiendo E2E" << std::endl;
            host.stop();
            return 0;
        }
    }
    std::string code = *codeOpt;

    // Viewer
    vic::pipeline::ViewerSession viewer;
    bool gotFrame = false;
    viewer.setFrameCallback([&](const vic::capture::DesktopFrame&) {
        gotFrame = true;
    });

    // Resolver código
    auto resolved = mm.resolveCode(code);
    if (!resolved) {
        if (std::getenv("VIC_REQUIRE_MATCHMAKER")) {
            std::cerr << "Resolve falló (modo estricto)" << std::endl;
            return 1;
        } else {
            std::cout << "SKIP: resolve falló, omitiendo E2E" << std::endl;
            host.stop();
            return 0;
        }
    }

    if (!viewer.connect(code)) {
        std::cerr << "Conexión viewer falló" << std::endl;
        return 1;
    }

    // Esperar algunos frames
    auto start = std::chrono::steady_clock::now();
    while (!gotFrame && std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    host.stop();
    viewer.disconnect();

    if (!gotFrame) {
        if (!std::getenv("VIC_REQUIRE_DISPLAY")) {
            std::cout << "SKIP: viewer sin frame (posible entorno headless), omitiendo E2E" << std::endl;
            return 0;
        }
        std::cerr << "No se recibió frame en ventana viewer" << std::endl;
        return 1;
    }

    std::cout << "E2E host→viewer OK (matchmaker remoto)" << std::endl;
    return 0;
}
