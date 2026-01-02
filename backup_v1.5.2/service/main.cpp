// VicViewer Windows Service
// Lanza un proceso helper en la sesion del usuario para capturar el escritorio
// El servicio actua como monitor y relanzador del proceso de captura

#include "Logger.h"

#include <Windows.h>
#include <WtsApi32.h>
#include <Userenv.h>
#include <string>
#include <atomic>
#include <thread>
#include <fstream>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Wtsapi32.lib")
#pragma comment(lib, "Userenv.lib")

namespace {

constexpr const wchar_t* SERVICE_NAME = L"VicViewerService";
constexpr const wchar_t* SERVICE_DISPLAY_NAME = L"VicViewer Remote Desktop Service";
constexpr const wchar_t* SERVICE_DESCRIPTION = L"Permite acceso remoto al escritorio incluyendo la pantalla de login";

SERVICE_STATUS g_serviceStatus = {};
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent = nullptr;
std::atomic_bool g_running{false};
HANDLE g_helperProcess = nullptr;
std::string g_fixedCode;

std::wstring getServiceDirectory() {
    wchar_t path[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len > 0) {
        std::wstring fullPath(path, len);
        size_t lastSlash = fullPath.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            return fullPath.substr(0, lastSlash + 1);
        }
    }
    return L"";
}

void loadConfig() {
    std::wstring configPath = getServiceDirectory() + L"vicviewer_service.cfg";
    std::ifstream file(configPath);
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            if (line.find("CODE=") == 0) {
                g_fixedCode = line.substr(5);
                vic::logging::global().log(vic::logging::Logger::Level::Info,
                    "[Service] Codigo fijo cargado: " + g_fixedCode);
            }
        }
    }
}

// Obtener la sesion activa del usuario (donde hay un escritorio visible)
DWORD getActiveSessionId() {
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        // No hay sesion de consola activa, buscar cualquier sesion activa
        WTS_SESSION_INFOW* sessions = nullptr;
        DWORD count = 0;
        if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
            for (DWORD i = 0; i < count; ++i) {
                if (sessions[i].State == WTSActive) {
                    sessionId = sessions[i].SessionId;
                    break;
                }
            }
            WTSFreeMemory(sessions);
        }
    }
    vic::logging::global().log(vic::logging::Logger::Level::Info,
        "[Service] Sesion activa detectada: " + std::to_string(sessionId));
    return sessionId;
}

// Lanzar proceso helper en la sesion del usuario
bool launchHelperInUserSession(DWORD sessionId) {
    if (g_helperProcess) {
        // Ya hay un proceso helper corriendo
        DWORD exitCode = 0;
        if (GetExitCodeProcess(g_helperProcess, &exitCode) && exitCode == STILL_ACTIVE) {
            return true; // Proceso sigue vivo
        }
        CloseHandle(g_helperProcess);
        g_helperProcess = nullptr;
    }

    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken)) {
        DWORD err = GetLastError();
        vic::logging::global().log(vic::logging::Logger::Level::Warning,
            "[Service] WTSQueryUserToken fallo, error: " + std::to_string(err) +
            " (puede que no haya usuario logueado)");
        return false;
    }

    // Duplicar el token para poder usarlo
    HANDLE duplicatedToken = nullptr;
    if (!DuplicateTokenEx(userToken, MAXIMUM_ALLOWED, nullptr, 
                          SecurityIdentification, TokenPrimary, &duplicatedToken)) {
        vic::logging::global().log(vic::logging::Logger::Level::Error,
            "[Service] DuplicateTokenEx fallo: " + std::to_string(GetLastError()));
        CloseHandle(userToken);
        return false;
    }
    CloseHandle(userToken);

    // Obtener el entorno del usuario
    LPVOID environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, duplicatedToken, FALSE)) {
        vic::logging::global().log(vic::logging::Logger::Level::Warning,
            "[Service] CreateEnvironmentBlock fallo, continuando sin entorno personalizado");
    }

    // Construir linea de comandos para VicViewer*.exe en modo servicio
    // Buscar el exe que empiece con VicViewer (puede ser VicViewer.exe o VicViewerXXXX.exe)
    std::wstring serviceDir = getServiceDirectory();
    std::wstring exePath;
    std::wstring fallbackExe;  // VicViewer.exe como último recurso
    
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW((serviceDir + L"VicViewer*.exe").c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        // Buscar un exe que NO sea VicViewerService.exe y preferir VicViewerXXXX.exe
        do {
            std::wstring fileName(findData.cFileName);
            if (fileName.find(L"Service") == std::wstring::npos) {
                // Si es VicViewer.exe exacto, guardar como fallback
                if (fileName == L"VicViewer.exe") {
                    fallbackExe = serviceDir + fileName;
                } else {
                    // Preferir cualquier VicViewerXXXX.exe (con código de empresa)
                    exePath = serviceDir + fileName;
                    break;
                }
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }
    
    if (exePath.empty() && !fallbackExe.empty()) {
        exePath = fallbackExe;
    }
    
    if (exePath.empty()) {
        // Fallback al nombre por defecto
        exePath = serviceDir + L"VicViewer.exe";
    }
    
    std::wstring cmdLine = L"\"" + exePath + L"\" --service-mode";
    if (!g_fixedCode.empty()) {
        cmdLine += L" --code=" + std::wstring(g_fixedCode.begin(), g_fixedCode.end());
    }

    vic::logging::global().log(vic::logging::Logger::Level::Info,
        "[Service] Lanzando helper: " + std::string(cmdLine.begin(), cmdLine.end()));

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");  // Escritorio del usuario


    PROCESS_INFORMATION pi = {};
    
    BOOL result = CreateProcessAsUserW(
        duplicatedToken,
        nullptr,
        const_cast<LPWSTR>(cmdLine.c_str()),
        nullptr,
        nullptr,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        environment,
        serviceDir.c_str(),
        &si,
        &pi
    );

    if (environment) {
        DestroyEnvironmentBlock(environment);
    }
    CloseHandle(duplicatedToken);

    if (!result) {
        vic::logging::global().log(vic::logging::Logger::Level::Error,
            "[Service] CreateProcessAsUserW fallo: " + std::to_string(GetLastError()));
        return false;
    }

    g_helperProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    vic::logging::global().log(vic::logging::Logger::Level::Info,
        "[Service] Helper lanzado exitosamente en sesion " + std::to_string(sessionId) +
        ", PID: " + std::to_string(pi.dwProcessId));

    return true;
}

// Terminar proceso helper si existe
void terminateHelper() {
    if (g_helperProcess) {
        vic::logging::global().log(vic::logging::Logger::Level::Info,
            "[Service] Terminando proceso helper...");
        TerminateProcess(g_helperProcess, 0);
        WaitForSingleObject(g_helperProcess, 5000);
        CloseHandle(g_helperProcess);
        g_helperProcess = nullptr;
    }
}

void setServiceStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    static DWORD checkPoint = 1;
    
    g_serviceStatus.dwCurrentState = state;
    g_serviceStatus.dwWin32ExitCode = exitCode;
    g_serviceStatus.dwWaitHint = waitHint;
    
    if (state == SERVICE_START_PENDING) {
        g_serviceStatus.dwControlsAccepted = 0;
    } else {
        g_serviceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }
    
    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) {
        g_serviceStatus.dwCheckPoint = 0;
    } else {
        g_serviceStatus.dwCheckPoint = checkPoint++;
    }
    
    SetServiceStatus(g_statusHandle, &g_serviceStatus);
}

void WINAPI serviceCtrlHandler(DWORD ctrlCode) {
    switch (ctrlCode) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            vic::logging::global().log(vic::logging::Logger::Level::Info, "[Service] Recibida senal de detencion");
            setServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 3000);
            g_running.store(false);
            if (g_stopEvent) {
                SetEvent(g_stopEvent);
            }
            break;
            
        case SERVICE_CONTROL_INTERROGATE:
            setServiceStatus(g_serviceStatus.dwCurrentState);
            break;
    }
}

void serviceMain() {
    vic::logging::global().log(vic::logging::Logger::Level::Info, "[Service] Iniciando servicio VicViewer...");
    
    // Cargar configuracion
    loadConfig();
    
    setServiceStatus(SERVICE_RUNNING);
    vic::logging::global().log(vic::logging::Logger::Level::Info, "[Service] Servicio en modo monitor activo");
    
    // Loop principal - monitorear y relanzar helper en sesion de usuario
    g_running.store(true);
    DWORD lastSessionId = 0;
    int checkCounter = 0;
    
    while (g_running.load()) {
        DWORD result = WaitForSingleObject(g_stopEvent, 2000);  // Check cada 2 segundos
        if (result == WAIT_OBJECT_0) {
            break;
        }
        
        // Detectar sesion activa
        DWORD sessionId = getActiveSessionId();
        
        // Si cambio la sesion, terminar helper anterior
        if (sessionId != lastSessionId && lastSessionId != 0) {
            vic::logging::global().log(vic::logging::Logger::Level::Info,
                "[Service] Cambio de sesion detectado: " + std::to_string(lastSessionId) + 
                " -> " + std::to_string(sessionId));
            terminateHelper();
        }
        lastSessionId = sessionId;
        
        // Intentar lanzar/verificar helper cada 2 segundos
        if (sessionId != 0 && sessionId != 0xFFFFFFFF) {
            if (!launchHelperInUserSession(sessionId)) {
                // Log solo cada 30 segundos si falla continuamente
                checkCounter++;
                if (checkCounter >= 15) {
                    checkCounter = 0;
                    vic::logging::global().log(vic::logging::Logger::Level::Warning,
                        "[Service] No se puede lanzar helper, esperando sesion de usuario...");
                }
            } else {
                checkCounter = 0;
            }
        }
    }
    
    // Terminar helper antes de salir
    terminateHelper();
    
    vic::logging::global().log(vic::logging::Logger::Level::Info, "[Service] Servicio detenido");
    setServiceStatus(SERVICE_STOPPED);
}

void WINAPI serviceMainEntry(DWORD argc, LPWSTR* argv) {
    (void)argc;
    (void)argv;
    
    g_statusHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, serviceCtrlHandler);
    if (!g_statusHandle) {
        return;
    }
    
    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwServiceSpecificExitCode = 0;
    
    setServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);
    
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        setServiceStatus(SERVICE_STOPPED, GetLastError());
        return;
    }
    
    serviceMain();
    
    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;
}

bool installService() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        wprintf(L"Error obteniendo ruta del ejecutable\n");
        return false;
    }
    
    SC_HANDLE scManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scManager) {
        wprintf(L"Error abriendo Service Control Manager (necesita permisos de administrador)\n");
        return false;
    }
    
    SC_HANDLE service = CreateServiceW(
        scManager,
        SERVICE_NAME,
        SERVICE_DISPLAY_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        path,
        nullptr,
        nullptr,
        nullptr,
        nullptr,  // LocalSystem account
        nullptr
    );
    
    if (!service) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            wprintf(L"El servicio ya existe\n");
        } else {
            wprintf(L"Error creando servicio: %lu\n", err);
        }
        CloseServiceHandle(scManager);
        return false;
    }
    
    // Configurar descripción
    SERVICE_DESCRIPTIONW desc = {};
    desc.lpDescription = const_cast<LPWSTR>(SERVICE_DESCRIPTION);
    ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &desc);
    
    // Configurar recuperación automática
    SERVICE_FAILURE_ACTIONSW failureActions = {};
    SC_ACTION actions[3] = {
        {SC_ACTION_RESTART, 60000},  // Reiniciar después de 1 minuto
        {SC_ACTION_RESTART, 60000},
        {SC_ACTION_RESTART, 60000}
    };
    failureActions.dwResetPeriod = 86400;  // Reset contador después de 1 día
    failureActions.cActions = 3;
    failureActions.lpsaActions = actions;
    ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failureActions);
    
    wprintf(L"Servicio instalado correctamente\n");
    wprintf(L"Para iniciarlo: net start VicViewerService\n");
    
    CloseServiceHandle(service);
    CloseServiceHandle(scManager);
    return true;
}

bool uninstallService() {
    SC_HANDLE scManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scManager) {
        wprintf(L"Error abriendo Service Control Manager\n");
        return false;
    }
    
    SC_HANDLE service = OpenServiceW(scManager, SERVICE_NAME, SERVICE_ALL_ACCESS);
    if (!service) {
        wprintf(L"Servicio no encontrado\n");
        CloseServiceHandle(scManager);
        return false;
    }
    
    // Detener si está corriendo
    SERVICE_STATUS status;
    if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        wprintf(L"Deteniendo servicio...\n");
        Sleep(2000);
    }
    
    if (!DeleteService(service)) {
        wprintf(L"Error eliminando servicio: %lu\n", GetLastError());
        CloseServiceHandle(service);
        CloseServiceHandle(scManager);
        return false;
    }
    
    wprintf(L"Servicio desinstalado correctamente\n");
    
    CloseServiceHandle(service);
    CloseServiceHandle(scManager);
    return true;
}

bool startService() {
    SC_HANDLE scManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scManager) {
        wprintf(L"Error abriendo Service Control Manager\n");
        return false;
    }
    
    SC_HANDLE service = OpenServiceW(scManager, SERVICE_NAME, SERVICE_START);
    if (!service) {
        wprintf(L"Servicio no encontrado\n");
        CloseServiceHandle(scManager);
        return false;
    }
    
    if (!StartServiceW(service, 0, nullptr)) {
        DWORD err = GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING) {
            wprintf(L"El servicio ya esta corriendo\n");
        } else {
            wprintf(L"Error iniciando servicio: %lu\n", err);
        }
        CloseServiceHandle(service);
        CloseServiceHandle(scManager);
        return false;
    }
    
    wprintf(L"Servicio iniciado\n");
    
    CloseServiceHandle(service);
    CloseServiceHandle(scManager);
    return true;
}

bool stopService() {
    SC_HANDLE scManager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scManager) {
        wprintf(L"Error abriendo Service Control Manager\n");
        return false;
    }
    
    SC_HANDLE service = OpenServiceW(scManager, SERVICE_NAME, SERVICE_STOP);
    if (!service) {
        wprintf(L"Servicio no encontrado\n");
        CloseServiceHandle(scManager);
        return false;
    }
    
    SERVICE_STATUS status;
    if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        wprintf(L"Error deteniendo servicio: %lu\n", GetLastError());
        CloseServiceHandle(service);
        CloseServiceHandle(scManager);
        return false;
    }
    
    wprintf(L"Servicio detenido\n");
    
    CloseServiceHandle(service);
    CloseServiceHandle(scManager);
    return true;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    // Inicializar logger
    vic::logging::global();
    
    if (argc > 1) {
        std::wstring arg = argv[1];
        
        if (arg == L"--install" || arg == L"-i") {
            return installService() ? 0 : 1;
        }
        else if (arg == L"--uninstall" || arg == L"-u") {
            return uninstallService() ? 0 : 1;
        }
        else if (arg == L"--start" || arg == L"-s") {
            return startService() ? 0 : 1;
        }
        else if (arg == L"--stop" || arg == L"-t") {
            return stopService() ? 0 : 1;
        }
        else if (arg == L"--help" || arg == L"-h") {
            wprintf(L"VicViewer Service\n\n");
            wprintf(L"Uso:\n");
            wprintf(L"  VicViewerService.exe --install    Instalar servicio\n");
            wprintf(L"  VicViewerService.exe --uninstall  Desinstalar servicio\n");
            wprintf(L"  VicViewerService.exe --start      Iniciar servicio\n");
            wprintf(L"  VicViewerService.exe --stop       Detener servicio\n");
            wprintf(L"\nConfigurar codigo fijo:\n");
            wprintf(L"  Crear archivo vicviewer_service.cfg junto al exe con:\n");
            wprintf(L"  CODE=MICODIGO\n");
            return 0;
        }
        else {
            wprintf(L"Opcion desconocida: %s\n", arg.c_str());
            wprintf(L"Use --help para ver opciones\n");
            return 1;
        }
    }
    
    // Sin argumentos = ejecutar como servicio
    SERVICE_TABLE_ENTRYW serviceTable[] = {
        {const_cast<LPWSTR>(SERVICE_NAME), serviceMainEntry},
        {nullptr, nullptr}
    };
    
    if (!StartServiceCtrlDispatcherW(serviceTable)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            // No estamos siendo ejecutados como servicio
            wprintf(L"Este programa debe ejecutarse como servicio de Windows.\n");
            wprintf(L"Use --help para ver opciones de instalacion.\n");
        }
        return 1;
    }
    
    return 0;
}
