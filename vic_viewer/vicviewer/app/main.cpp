#include "VicViewerUI.h"

#include "AppContext.h"
#include "HostSession.h"
#include "MatchmakerClient.h"

#include <algorithm>
#include <cwctype>
#include <shellapi.h>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <tlhelp32.h>   // Para CreateToolhelp32Snapshot

#include "Logger.h"

#pragma comment(lib, "Shell32.lib")

namespace {

// =============================================================================
// PROTECCION ANTI-INGENIERIA INVERSA
// =============================================================================

// Detecta si hay un debugger conectado
bool isDebuggerPresent() {
    // Método 1: API de Windows
    if (IsDebuggerPresent()) {
        return true;
    }
    
    // Método 2: CheckRemoteDebuggerPresent
    BOOL debuggerPresent = FALSE;
    if (CheckRemoteDebuggerPresent(GetCurrentProcess(), &debuggerPresent) && debuggerPresent) {
        return true;
    }
    
    // Método 3: Timing check (debuggers hacen las cosas más lentas)
    LARGE_INTEGER freq, start, end;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    
    // Operación simple que debería ser instantánea
    volatile int dummy = 0;
    for (int i = 0; i < 100; i++) dummy++;
    
    QueryPerformanceCounter(&end);
    double elapsed = (double)(end.QuadPart - start.QuadPart) / freq.QuadPart;
    
    // Si tarda más de 50ms, hay algo raro (normalmente < 1ms)
    if (elapsed > 0.05) {
        return true;
    }
    
    return false;
}

// Detecta herramientas comunes de análisis
bool isAnalysisTool() {
    const wchar_t* suspiciousProcesses[] = {
        L"ollydbg.exe", L"x64dbg.exe", L"x32dbg.exe", L"ida.exe", L"ida64.exe",
        L"idaq.exe", L"idaq64.exe", L"windbg.exe", L"processhacker.exe",
        L"procmon.exe", L"procmon64.exe", L"wireshark.exe", L"fiddler.exe",
        L"charles.exe", L"cheatengine", L"dnspy.exe", L"dotpeek", L"ghidra"
    };
    
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return false;
    
    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    
    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            std::wstring procName = pe32.szExeFile;
            std::transform(procName.begin(), procName.end(), procName.begin(), ::tolower);
            
            for (const auto& suspicious : suspiciousProcesses) {
                if (procName.find(suspicious) != std::wstring::npos) {
                    CloseHandle(hSnapshot);
                    return true;
                }
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    
    CloseHandle(hSnapshot);
    return false;
}

// Verifica integridad básica del ejecutable
void performSecurityChecks() {
    #ifndef _DEBUG
    // Solo en Release
    if (isDebuggerPresent()) {
        // Comportamiento silencioso - simplemente salir o comportarse diferente
        ExitProcess(0);
    }
    
    if (isAnalysisTool()) {
        // Mostrar mensaje genérico y salir
        MessageBoxW(NULL, 
            L"No se puede iniciar la aplicación.\n\n"
            L"Por favor cierre otras aplicaciones e intente de nuevo.",
            L"Error de inicialización", MB_ICONERROR);
        ExitProcess(0);
    }
    #endif
}

// =============================================================================

std::wstring toLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

struct CommandLineArgs {
    vic::ui::LaunchOptions options;
    bool serviceMode = false;
    std::wstring fixedCode;
    std::wstring testPassword;  // Para probar validacion
};

CommandLineArgs parseCommandLine() {
    CommandLineArgs args;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return args;
    }

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg.size() < 2 || arg[0] != L'-') {
            continue;
        }

        std::wstring key = arg;
        std::wstring value;
        auto eqPos = arg.find(L'=');
        if (eqPos != std::wstring::npos) {
            key = arg.substr(0, eqPos);
            value = arg.substr(eqPos + 1);
        }

        auto lowerKey = toLower(key);

        auto consumeNext = [&](int& index) -> std::wstring {
            if (eqPos != std::wstring::npos) {
                return value;
            }
            if (index + 1 < argc && argv[index + 1][0] != L'-') {
                ++index;
                return std::wstring(argv[index]);
            }
            return L"";
        };

        if (lowerKey == L"--mode") {
            auto modeValue = consumeNext(i);
            auto lowerValue = toLower(modeValue);
            if (lowerValue == L"host") {
                args.options.mode = vic::ui::LaunchOptions::Mode::Host;
            } else if (lowerValue == L"viewer") {
                args.options.mode = vic::ui::LaunchOptions::Mode::Viewer;
            }
        } else if (lowerKey == L"--code" || lowerKey == L"--session-code") {
            args.options.sessionCode = consumeNext(i);
            args.fixedCode = args.options.sessionCode;
        } else if (lowerKey == L"--minimize" || lowerKey == L"--minimized") {
            args.options.minimizeOnStart = true;
        } else if (lowerKey == L"--service-mode") {
            args.serviceMode = true;
        } else if (lowerKey == L"--test-pwd") {
            args.testPassword = consumeNext(i);
        }
    }

    LocalFree(argv);
    return args;
}

// Modo servicio: correr headless como host con registro en matchmaker
int runServiceMode(const std::wstring& fixedCode) {
    vic::logging::global().log(vic::logging::Logger::Level::Info, 
        "[ServiceMode] VicViewer iniciado en modo servicio");
    
    // Crear sesión de host - HostSession maneja TODO internamente
    auto hostSession = std::make_unique<vic::pipeline::HostSession>();
    
    // Configurar URL del matchmaker (IP directa porque DNS no resuelve)
    hostSession->setMatchmakerUrl(L"http://38.242.234.197:8080");
    
    // Configurar código fijo si está definido
    if (!fixedCode.empty()) {
        std::string code(fixedCode.begin(), fixedCode.end());
        hostSession->setFixedCode(code);
        vic::logging::global().log(vic::logging::Logger::Level::Info,
            "[ServiceMode] Usando codigo fijo: " + code);
    }
    
    // Iniciar host - el signalingLoop interno hace registro y polling de answers
    if (!hostSession->start()) {
        vic::logging::global().log(vic::logging::Logger::Level::Error, 
            "[ServiceMode] Error al iniciar HostSession");
        return 1;
    }
    
    vic::logging::global().log(vic::logging::Logger::Level::Info, 
        "[ServiceMode] HostSession iniciado - signalingLoop activo");
    
    if (auto info = hostSession->connectionInfo()) {
        vic::logging::global().log(vic::logging::Logger::Level::Info, 
            "[ServiceMode] Codigo de conexion: " + info->code);
    }
    
    // Mantener el proceso vivo - HostSession hace todo en background
    while (true) {
        Sleep(10000);
    }
    
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    // VERIFICACION DE SEGURIDAD ANTI-DEBUG
    performSecurityChecks();
    
#ifdef _DEBUG
    // Abrir consola en modo debug
    AllocConsole();
    FILE* stream;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
#endif

    vic::AppContext context;
    vic::logging::global().log(vic::logging::Logger::Level::Info, "VicViewer iniciado");
    
    const auto args = parseCommandLine();
    
    // Debug: mostrar si se detectó modo servicio
    vic::logging::global().log(vic::logging::Logger::Level::Info, 
        "Modo servicio: " + std::string(args.serviceMode ? "SI" : "NO"));
    vic::logging::global().log(vic::logging::Logger::Level::Info, 
        "testPassword vacio: " + std::string(args.testPassword.empty() ? "SI" : "NO"));
    
    // Modo de prueba de validacion de password
    if (!args.testPassword.empty()) {
        // Extraer companyCode del nombre del exe
        wchar_t exePath[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring path(exePath);
        auto slash = path.find_last_of(L"\\/");
        std::wstring exeName = (slash != std::wstring::npos) ? path.substr(slash + 1) : path;
        auto dot = exeName.rfind(L'.');
        if (dot != std::wstring::npos) exeName = exeName.substr(0, dot);
        
        std::string companyCode;
        const std::wstring prefix = L"VicViewer";
        if (exeName.find(prefix) == 0 && exeName.length() > prefix.length()) {
            std::wstring codeW = exeName.substr(prefix.length());
            companyCode = std::string(codeW.begin(), codeW.end());
        }
        
        vic::logging::global().log(vic::logging::Logger::Level::Info, 
            "[TEST] Probando validacion de password. CompanyCode: " + companyCode);
        
        auto matchmaker = std::make_unique<vic::matchmaking::MatchmakerClient>(L"https://vicviewer.com");
        matchmaker->setCompanyCode(companyCode);
        std::string pwd(args.testPassword.begin(), args.testPassword.end());
        auto result = matchmaker->validateServicePassword(pwd);
        if (result) {
            std::string msg = "valid=" + std::string(result->valid ? "true" : "false");
            if (!result->userName.empty()) msg += ", userName=" + result->userName;
            if (!result->companyName.empty()) msg += ", company=" + result->companyName;
            vic::logging::global().log(vic::logging::Logger::Level::Info, "[TEST] Resultado: " + msg);
            MessageBoxA(nullptr, msg.c_str(), result->valid ? "VALIDACION OK" : "VALIDACION FALLIDA", MB_OK);
        } else {
            vic::logging::global().log(vic::logging::Logger::Level::Error, "[TEST] No se obtuvo respuesta");
            MessageBoxW(nullptr, L"Sin respuesta del servidor", L"Error", MB_ICONERROR);
        }
        return 0;
    }
    
    // Si es modo servicio, correr headless
    if (args.serviceMode) {
        return runServiceMode(args.fixedCode);
    }
    
    // Modo normal con UI
    return vic::ui::run(hInstance, nCmdShow, context, args.options);
}
