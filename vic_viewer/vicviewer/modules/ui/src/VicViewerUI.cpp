#include "VicViewerUI.h"

#include "HostSession.h"
#include "Logger.h"
#include "MatchmakerClient.h"
#include "StreamConfig.h"
#include "ViewerSession.h"
#include "vic/ui/AntiAbuse.h"

#include <windows.h>
#include <winsock2.h>   // Para obtener IP local (antes de windows.h conceptualmente)
#include <ws2tcpip.h>   // Para inet_ntop
#include <iphlpapi.h>   // Para GetAdaptersAddresses
#include <objidl.h>     // Necesario antes de gdiplus.h
#include <gdiplus.h>    // Para cargar imagenes PNG
#include <CommCtrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <winioctl.h>   // Para IOCTL_STORAGE_QUERY_PROPERTY
#include <winhttp.h>    // Para descarga de banners

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Iphlpapi.lib")

#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#include <optional>
#include <algorithm>
#include <fstream>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "Gdiplus.lib")

namespace vic::ui {

namespace {

// Custom colors for modern UI - TEMA AZUL OSCURO (SaaS Style)
constexpr COLORREF COLOR_BG = RGB(15, 23, 42);           // Azul muy oscuro (slate-900)
constexpr COLORREF COLOR_BG_LIGHT = RGB(30, 41, 59);     // Azul oscuro (slate-800)
constexpr COLORREF COLOR_ACCENT = RGB(59, 130, 246);     // Azul brillante (blue-500)
constexpr COLORREF COLOR_TEXT = RGB(248, 250, 252);      // Blanco suave (slate-50)
constexpr COLORREF COLOR_TEXT_DIM = RGB(148, 163, 184);  // Gris azulado (slate-400)
constexpr COLORREF COLOR_SUCCESS = RGB(34, 197, 94);     // Verde (green-500)
constexpr COLORREF COLOR_TAB_ACTIVE = RGB(51, 65, 85);   // Azul medio (slate-700)
constexpr COLORREF COLOR_TAB_INACTIVE = RGB(30, 41, 59); // Azul oscuro (slate-800)

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_VIEWER_CONNECTED = WM_APP + 3;
constexpr UINT WM_VIEWER_TIMEOUT = WM_APP + 4;

// Timer IDs
constexpr UINT_PTR TIMER_VIEWER_CONNECT_TIMEOUT = 5001;
constexpr UINT_PTR TIMER_VIEWER_FREE_SESSION = 5002;
constexpr int VIEWER_CONNECT_TIMEOUT_MS = 90000;  // 90 segundos
constexpr int VIEWER_FREE_SESSION_MS = 300000;  // 5 minutos para modo FREE

// IDs para menú contextual del tray
constexpr UINT IDM_TRAY_OPEN = 4001;
constexpr UINT IDM_TRAY_CLOSE = 4002;

// ID del icono de recursos (definido en app.rc)
#define IDI_APPICON 101

constexpr int WINDOW_WIDTH = 500;
constexpr int WINDOW_HEIGHT = 400;
constexpr int TAB_HEIGHT = 40;
constexpr int MARGIN = 20;

WNDPROC g_originalCanvasProc = nullptr;
HBRUSH g_bgBrush = nullptr;
HBRUSH g_bgLightBrush = nullptr;
HFONT g_fontNormal = nullptr;
HFONT g_fontBold = nullptr;
HFONT g_fontCode = nullptr;

enum class TabMode { Host = 0, Viewer = 1, Service = 2 };

struct MainWindowState {
    std::unique_ptr<vic::pipeline::HostSession> hostSession{std::make_unique<vic::pipeline::HostSession>()};
    std::unique_ptr<vic::pipeline::ViewerSession> viewerSession{std::make_unique<vic::pipeline::ViewerSession>()};
    std::unique_ptr<vic::matchmaking::MatchmakerClient> matchmaker{std::make_unique<vic::matchmaking::MatchmakerClient>(L"https://vicviewer.com")};

    HWND mainWindow = nullptr;
    TabMode currentTab = TabMode::Host;
    
    // Código fijo opcional (desde línea de comandos)
    std::string fixedCode;
    
    // Company code extraído del nombre del ejecutable (ej: VicViewerABC1.exe -> "ABC1")
    std::string companyCode;
    // Serial del disco duro para modo free
    std::string diskSerial;
    // Client ID (legacy, alias de companyCode)
    std::string clientId;
    bool autoStartPending = false;  // Para auto-iniciar después de crear ventana
    bool servicePasswordValidated = false;  // Si ya se valido la clave de servicio para conectar
    
    // Modo free
    bool isFreeMode = false;
    int freeSessionMaxMs = 0;  // Duración máxima en modo free (ms)
    std::chrono::steady_clock::time_point freeSessionStart;  // Inicio de sesión free
    UINT_PTR freeSessionTimer = 0;  // Timer para desconectar en modo free
    
    // Tab buttons (custom drawn)
    RECT tabHostRect{};
    RECT tabViewerRect{};
    RECT tabServiceRect{};
    
    // Host controls
    HWND hostCodeEdit = nullptr;
    HWND hostLocalIPLabel = nullptr;  // Label para IP local (conexión LAN)
    HWND hostButton = nullptr;
    HWND hostStatus = nullptr;
    HWND hostQualityCombo = nullptr;  // Selector de calidad
    HWND hostQualityLabel = nullptr;
    HWND hostMetricsLabel = nullptr;  // FPS/bitrate display
    bool hostRunning = false;
    bool hostHadViewerConnected = false;  // Para detectar desconexion del viewer
    std::string activeCode;  // Código activo para heartbeat/disconnect

    // Viewer controls  
    HWND viewerCodeEdit = nullptr;
    HWND viewerButton = nullptr;
    HWND viewerCanvas = nullptr;
    bool viewerConnected = false;
    
    // Viewer FREE mode (quien paga es el viewer)
    bool viewerFreeMode = false;
    std::chrono::steady_clock::time_point viewerFreeStart;
    UINT_PTR viewerFreeTimer = 0;

    // Service controls
    HWND serviceCodeEdit = nullptr;
    HWND serviceCodeLabel = nullptr;
    HWND serviceGenerateBtn = nullptr;   // Botón para generar código
    HWND servicePasswordEdit = nullptr;  // Campo para contraseña de servicio
    HWND servicePasswordLabel = nullptr;
    HWND serviceAutoReconnect = nullptr; // Checkbox reconectar al reinicio
    HWND serviceNoAutoCode = nullptr;    // Checkbox no generar codigo al inicio
    HWND serviceInstallBtn = nullptr;
    HWND serviceUninstallBtn = nullptr;
    HWND serviceStartBtn = nullptr;
    HWND serviceStopBtn = nullptr;
    HWND serviceStatus = nullptr;
    HWND serviceRefreshBtn = nullptr;
    
    // Banner
    HWND bannerStatic = nullptr;
    HBITMAP bannerBitmap = nullptr;
    std::wstring bannerPath;  // Ruta local del banner

    // Tray
    bool trayVisible = false;
    bool hiddenToTrayOnce = false;  // Flag para evitar re-ocultar cuando usuario restaura
    HICON trayIcon = nullptr;
    
    // Heartbeat
    UINT_PTR heartbeatTimer = 0;

    std::optional<vic::capture::DesktopFrame> lastFrame;
    std::mutex frameMutex;
};

// Utility functions
std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

// =========== LAN IP ADDRESS FUNCTION ===========
// Obtiene la IP local del equipo para conexiones LAN directas
std::string getLocalIPAddress() {
    std::string localIP = "No disponible";
    
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return localIP;
    }
    
    // Usar GetAdaptersAddresses para obtener IPs de adaptadores activos
    ULONG bufferSize = 15000;
    std::vector<BYTE> buffer(bufferSize);
    PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bufferSize);
    
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufferSize);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &bufferSize);
    }
    
    if (result == NO_ERROR) {
        // Buscar primer adaptador activo con IP válida (no loopback)
        for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
            // Solo adaptadores activos (Ethernet o WiFi)
            if (adapter->OperStatus != IfOperStatusUp) continue;
            if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD && 
                adapter->IfType != IF_TYPE_IEEE80211) continue;
            
            for (PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress; 
                 unicast != nullptr; unicast = unicast->Next) {
                
                if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                    sockaddr_in* sockaddr = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                    char ipStr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sockaddr->sin_addr, ipStr, INET_ADDRSTRLEN);
                    
                    // Ignorar loopback
                    if (strcmp(ipStr, "127.0.0.1") != 0) {
                        localIP = ipStr;
                        WSACleanup();
                        return localIP;
                    }
                }
            }
        }
    }
    
    WSACleanup();
    return localIP;
}

// Extrae el company_code del nombre del ejecutable
// Ej: "VicViewerABC1.exe" -> "ABC1", "VicViewer.exe" -> ""
std::string extractCompanyCodeFromExeName() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    
    // Obtener solo el nombre del archivo
    std::wstring fullPath(path);
    size_t lastSlash = fullPath.find_last_of(L"\\/");
    std::wstring fileName = (lastSlash != std::wstring::npos) ? fullPath.substr(lastSlash + 1) : fullPath;
    
    // Quitar extensión .exe
    size_t dotPos = fileName.rfind(L'.');
    if (dotPos != std::wstring::npos) {
        fileName = fileName.substr(0, dotPos);
    }
    
    // Buscar el código después de "VicViewer"
    // VicViewerABC1 -> extraer "ABC1"
    std::string companyCode;
    std::string name = wideToUtf8(fileName);
    
    // El prefijo es "VicViewer" (9 caracteres)
    const std::string prefix = "VicViewer";
    if (name.length() > prefix.length() && name.substr(0, prefix.length()) == prefix) {
        companyCode = name.substr(prefix.length());
        // Convertir a mayúsculas
        for (char& c : companyCode) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    
    vic::logging::global().log(vic::logging::Logger::Level::Info, 
        "Ejecutable: " + name + " -> CompanyCode: " + (companyCode.empty() ? "(ninguno - modo free)" : companyCode));
    
    return companyCode;
}

// Obtiene el serial del disco duro físico (no de partición)
std::string getPhysicalDiskSerial() {
    std::string serial = "UNKNOWN";
    
    HANDLE hDevice = CreateFileW(L"\\\\.\\PhysicalDrive0", 
        0, FILE_SHARE_READ | FILE_SHARE_WRITE, 
        NULL, OPEN_EXISTING, 0, NULL);
    
    if (hDevice != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY query{};
        query.PropertyId = StorageDeviceProperty;
        query.QueryType = PropertyStandardQuery;
        
        char buffer[1024] = {0};
        DWORD bytesReturned = 0;
        
        if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
            &query, sizeof(query), buffer, sizeof(buffer), &bytesReturned, NULL)) {
            
            STORAGE_DEVICE_DESCRIPTOR* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer);
            if (desc->SerialNumberOffset > 0) {
                serial = std::string(buffer + desc->SerialNumberOffset);
                // Limpiar espacios
                serial.erase(std::remove_if(serial.begin(), serial.end(), 
                    [](char c) { return std::isspace(static_cast<unsigned char>(c)); }), serial.end());
            }
        }
        CloseHandle(hDevice);
    }
    
    vic::logging::global().log(vic::logging::Logger::Level::Info, 
        "Disk serial: " + serial.substr(0, 8) + "...");
    
    return serial;
}

// Alias para compatibilidad
std::string extractClientIdFromExeName() {
    return extractCompanyCodeFromExeName();
}

// =========== BANNER FUNCTIONS ===========
// Dimensiones del banner: 460x60 pixels
constexpr int BANNER_WIDTH = 460;
constexpr int BANNER_HEIGHT = 60;
constexpr const wchar_t* BANNER_DEFAULT_CODE = L"0000";
constexpr const wchar_t* BANNER_SERVER_URL = L"vicviewer.com";
constexpr const wchar_t* BANNER_PATH = L"/banners/";

// Obtiene el directorio de banners (junto al exe)
std::wstring getBannerDirectory() {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t lastSlash = dir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        dir = dir.substr(0, lastSlash + 1);
    }
    dir += L"banners\\";
    return dir;
}

// Descarga un archivo desde el servidor
bool downloadFile(const std::wstring& host, const std::wstring& path, const std::wstring& localPath) {
    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return false;
    
    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }
    
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, 
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    
    if (!WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    
    // Verificar codigo de respuesta
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, 
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    
    if (statusCode != 200) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    
    // Crear directorio si no existe
    std::wstring dir = getBannerDirectory();
    CreateDirectoryW(dir.c_str(), nullptr);
    
    // Abrir archivo para escribir
    FILE* f = _wfopen(localPath.c_str(), L"wb");
    if (!f) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    
    // Leer y escribir datos
    char buffer[8192];
    DWORD bytesRead = 0;
    while (WinHttpReadData(request, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        fwrite(buffer, 1, bytesRead, f);
    }
    
    fclose(f);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    
    return true;
}

// Carga una imagen PNG usando GDI+
HBITMAP loadPngImage(const std::wstring& path) {
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);
    
    HBITMAP hBitmap = nullptr;
    Gdiplus::Bitmap* bitmap = Gdiplus::Bitmap::FromFile(path.c_str());
    if (bitmap && bitmap->GetLastStatus() == Gdiplus::Ok) {
        bitmap->GetHBITMAP(Gdiplus::Color(255, 255, 255), &hBitmap);
        delete bitmap;
    }
    
    // No llamamos GdiplusShutdown aqui porque puede causar problemas si se llama varias veces
    // En produccion, se llamaria al cerrar la aplicacion
    
    return hBitmap;
}

// Obtiene la ruta local del banner para un company code
std::wstring getBannerLocalPath(const std::wstring& companyCode) {
    return getBannerDirectory() + companyCode + L".png";
}

// Descarga el banner si no existe localmente
// Retorna la ruta del banner a usar (empresa si pagado, default si no)
std::wstring ensureBannerExists(const std::string& companyCode, bool isPaidAccount) {
    std::wstring bannerDir = getBannerDirectory();
    CreateDirectoryW(bannerDir.c_str(), nullptr);
    
    std::wstring defaultBanner = bannerDir + BANNER_DEFAULT_CODE + L".png";
    
    // Siempre asegurarse que el banner por defecto existe
    if (GetFileAttributesW(defaultBanner.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring serverPath = std::wstring(BANNER_PATH) + BANNER_DEFAULT_CODE + L".png";
        downloadFile(BANNER_SERVER_URL, serverPath, defaultBanner);
    }
    
    // Si no hay company code o no es cuenta pagada, usar default
    if (companyCode.empty() || !isPaidAccount) {
        return defaultBanner;
    }
    
    // Intentar obtener banner de la empresa
    std::wstring companyCodeW = utf8ToWide(companyCode);
    std::wstring companyBanner = bannerDir + companyCodeW + L".png";
    
    // Si ya existe, usarlo
    if (GetFileAttributesW(companyBanner.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return companyBanner;
    }
    
    // Intentar descargar banner de la empresa
    std::wstring serverPath = std::wstring(BANNER_PATH) + companyCodeW + L".png";
    if (downloadFile(BANNER_SERVER_URL, serverPath, companyBanner)) {
        return companyBanner;
    }
    
    // Si falla, usar default
    return defaultBanner;
}

// Actualiza el banner mostrado
void updateBanner(MainWindowState* state, bool isPaidAccount) {
    if (!state || !state->bannerStatic) return;
    
    std::wstring bannerPath = ensureBannerExists(state->companyCode, isPaidAccount);
    
    // Liberar bitmap anterior
    if (state->bannerBitmap) {
        DeleteObject(state->bannerBitmap);
        state->bannerBitmap = nullptr;
    }
    
    // Cargar nuevo banner
    state->bannerBitmap = loadPngImage(bannerPath);
    if (state->bannerBitmap) {
        SendMessage(state->bannerStatic, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)state->bannerBitmap);
        state->bannerPath = bannerPath;
    }
}

// =========== END BANNER FUNCTIONS ===========

// Variable global para el dialogo de password
static HWND g_pwdEdit = nullptr;
static bool g_pwdOkClicked = false;
static wchar_t g_pwdBuffer[16] = {0};

// Window procedure para el dialogo de password
LRESULT CALLBACK PasswordDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            GetWindowTextW(g_pwdEdit, g_pwdBuffer, 16);
            if (wcslen(g_pwdBuffer) == 5) {
                g_pwdOkClicked = true;
                PostQuitMessage(0);
            } else {
                MessageBoxW(hwnd, L"La clave debe tener 5 caracteres.", L"Error", MB_ICONWARNING);
                SetWindowTextW(g_pwdEdit, L"");
                SetFocus(g_pwdEdit);
            }
        } else if (LOWORD(wParam) == IDCANCEL) {
            g_pwdOkClicked = false;
            PostQuitMessage(0);
        }
        return 0;
    case WM_CLOSE:
        g_pwdOkClicked = false;
        PostQuitMessage(0);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG:
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Validacion de clave de servicio al inicio
// Muestra un dialogo modal para pedir la clave cuando hay companyCode
// Retorna true si la validacion fue exitosa, false si se cancela o falla
bool validateServicePasswordOnStartup(const std::string& companyCode) {
    if (companyCode.empty()) return true;  // Modo free, no requiere validacion
    
    // Inicializar recursos GDI localmente si no existen
    if (!g_fontCode) {
        g_fontCode = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
    }
    if (!g_fontNormal) {
        g_fontNormal = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    
    // Crear matchmaker temporal para la validacion
    auto matchmaker = std::make_unique<vic::matchmaking::MatchmakerClient>(L"https://vicviewer.com");
    matchmaker->setCompanyCode(companyCode);
    
    vic::logging::global().log(vic::logging::Logger::Level::Info, 
        "[UI] Validacion de clave de servicio requerida para: " + companyCode);
    
    bool validated = false;
    int attempts = 0;
    const int maxAttempts = 3;
    
    // Registrar clase de ventana para el dialogo
    const wchar_t* dlgClassName = L"VicViewerPwdDlg";
    WNDCLASSEXW wcDlg = {};
    wcDlg.cbSize = sizeof(wcDlg);
    wcDlg.lpfnWndProc = PasswordDlgProc;
    wcDlg.hInstance = GetModuleHandle(nullptr);
    wcDlg.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcDlg.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcDlg.lpszClassName = dlgClassName;
    RegisterClassExW(&wcDlg);
    
    while (!validated && attempts < maxAttempts) {
        g_pwdOkClicked = false;
        g_pwdBuffer[0] = L'\0';
        
        // Centrar en pantalla
        int dlgW = 360, dlgH = 180;
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int posX = (screenW - dlgW) / 2;
        int posY = (screenH - dlgH) / 2;
        
        // Crear ventana del dialogo
        HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
            dlgClassName, L"Acceso empresarial",
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
            posX, posY, dlgW, dlgH,
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        
        if (!hDlg) {
            MessageBoxW(nullptr, L"Error al crear dialogo.", L"Error", MB_ICONERROR);
            return false;
        }
        
        // Texto de instruccion
        HWND hLabel2 = CreateWindowW(L"STATIC", L"Ingrese su clave de servicio:",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 15, 330, 20, hDlg, nullptr, GetModuleHandle(nullptr), nullptr);
        SendMessage(hLabel2, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
        
        // Campo de texto
        g_pwdEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_UPPERCASE,
            10, 40, 220, 30, hDlg, (HMENU)100, GetModuleHandle(nullptr), nullptr);
        SendMessage(g_pwdEdit, EM_SETLIMITTEXT, 5, 0);
        SendMessage(g_pwdEdit, WM_SETFONT, (WPARAM)g_fontCode, TRUE);
        
        // Boton Validar
        HWND hBtnOk = CreateWindowW(L"BUTTON", L"Validar",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            240, 40, 100, 30, hDlg, (HMENU)IDOK, GetModuleHandle(nullptr), nullptr);
        SendMessage(hBtnOk, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
        
        // Texto de ayuda
        HWND hLabel3 = CreateWindowW(L"STATIC", L"Obtenga su clave en vicviewer.com",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 80, 330, 20, hDlg, nullptr, GetModuleHandle(nullptr), nullptr);
        SendMessage(hLabel3, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
        
        // Boton Cancelar
        HWND hBtnCancel = CreateWindowW(L"BUTTON", L"Cancelar",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            240, 80, 100, 28, hDlg, (HMENU)IDCANCEL, GetModuleHandle(nullptr), nullptr);
        SendMessage(hBtnCancel, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
        
        SetFocus(g_pwdEdit);
        
        // Message loop
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            if (!IsDialogMessage(hDlg, &msg)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }
        
        DestroyWindow(hDlg);
        
        if (g_pwdOkClicked) {
            std::string pwd = wideToUtf8(g_pwdBuffer);
            vic::logging::global().log(vic::logging::Logger::Level::Info, 
                "[UI] Validando clave para " + companyCode + ": " + pwd);
            
            auto result = matchmaker->validateServicePassword(pwd);
            if (result && result->valid) {
                validated = true;
                vic::logging::global().log(vic::logging::Logger::Level::Info, 
                    "[UI] Clave validada OK para " + companyCode);
                
                // Mensaje de bienvenida simple
                MessageBoxW(nullptr, L"Acceso autorizado.\n\nPuede conectarse como visor.", 
                    L"Bienvenido", MB_ICONINFORMATION);
            } else {
                attempts++;
                std::wstring errMsg = L"Clave incorrecta.";
                if (result && !result->error.empty()) {
                    errMsg = utf8ToWide(result->error);
                } else if (result && !result->message.empty()) {
                    errMsg = utf8ToWide(result->message);
                }
                if (attempts < maxAttempts) {
                    errMsg += L"\n\nIntentos restantes: " + std::to_wstring(maxAttempts - attempts);
                    MessageBoxW(nullptr, errMsg.c_str(), L"Error", MB_ICONWARNING);
                } else {
                    MessageBoxW(nullptr, L"Demasiados intentos fallidos.", 
                        L"Acceso Denegado", MB_ICONERROR);
                }
            }
        } else {
            // Usuario cancelo
            vic::logging::global().log(vic::logging::Logger::Level::Info, 
                "[UI] Validacion cancelada por usuario");
            break;
        }
    }
    
    UnregisterClassW(dlgClassName, GetModuleHandle(nullptr));
    
    if (!validated) {
        vic::logging::global().log(vic::logging::Logger::Level::Warning, 
            "[UI] Validacion de clave fallida para " + companyCode);
    }
    
    return validated;
}

void setWindowUserData(HWND hwnd, MainWindowState* state) {
    SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
}

MainWindowState* getWindowState(HWND hwnd) {
    return reinterpret_cast<MainWindowState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
}

void initGdiResources() {
    if (!g_bgBrush) g_bgBrush = CreateSolidBrush(COLOR_BG);
    if (!g_bgLightBrush) g_bgLightBrush = CreateSolidBrush(COLOR_BG_LIGHT);
    if (!g_fontNormal) g_fontNormal = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, 
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!g_fontBold) g_fontBold = CreateFontW(14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!g_fontCode) g_fontCode = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
}

void cleanupGdiResources() {
    if (g_bgBrush) { DeleteObject(g_bgBrush); g_bgBrush = nullptr; }
    if (g_bgLightBrush) { DeleteObject(g_bgLightBrush); g_bgLightBrush = nullptr; }
    if (g_fontNormal) { DeleteObject(g_fontNormal); g_fontNormal = nullptr; }
    if (g_fontBold) { DeleteObject(g_fontBold); g_fontBold = nullptr; }
    if (g_fontCode) { DeleteObject(g_fontCode); g_fontCode = nullptr; }
}

// Helper para obtener icono de la app
HICON getAppIcon(HINSTANCE hInst = nullptr) {
    if (!hInst) hInst = GetModuleHandle(nullptr);
    HICON icon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_APPICON));
    if (!icon) icon = LoadIcon(nullptr, IDI_APPLICATION); // Fallback
    return icon;
}

// Tray functions
void addTrayIcon(HWND hwnd, MainWindowState* state, const std::wstring& tip) {
    if (!state) return;
    NOTIFYICONDATA nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = getAppIcon();
    wcsncpy_s(nid.szTip, std::size(nid.szTip), tip.c_str(), _TRUNCATE);

    if (state->trayVisible) {
        Shell_NotifyIcon(NIM_MODIFY, &nid);
    } else if (Shell_NotifyIcon(NIM_ADD, &nid)) {
        state->trayVisible = true;
        state->trayIcon = nid.hIcon;
    }
}

void removeTrayIcon(MainWindowState* state) {
    if (!state || !state->trayVisible) return;
    NOTIFYICONDATA nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = state->mainWindow;
    nid.uID = 1;
    Shell_NotifyIcon(NIM_DELETE, &nid);
    state->trayVisible = false;
}

void hideToTray(HWND hwnd, MainWindowState* state) {
    addTrayIcon(hwnd, state, L"Vicviewer - Compartiendo pantalla");
    ShowWindow(hwnd, SW_HIDE);
    state->hiddenToTrayOnce = true;
}

// Drawing functions
void drawTabs(HDC hdc, MainWindowState* state, int width) {
    int tabWidth = width / 3;
    
    state->tabHostRect = {0, 0, tabWidth, TAB_HEIGHT};
    state->tabViewerRect = {tabWidth, 0, tabWidth * 2, TAB_HEIGHT};
    state->tabServiceRect = {tabWidth * 2, 0, width, TAB_HEIGHT};
    
    // Host tab
    HBRUSH hostBrush = CreateSolidBrush(state->currentTab == TabMode::Host ? COLOR_TAB_ACTIVE : COLOR_TAB_INACTIVE);
    FillRect(hdc, &state->tabHostRect, hostBrush);
    DeleteObject(hostBrush);
    
    // Viewer tab
    HBRUSH viewerBrush = CreateSolidBrush(state->currentTab == TabMode::Viewer ? COLOR_TAB_ACTIVE : COLOR_TAB_INACTIVE);
    FillRect(hdc, &state->tabViewerRect, viewerBrush);
    DeleteObject(viewerBrush);
    
    // Service tab
    HBRUSH serviceBrush = CreateSolidBrush(state->currentTab == TabMode::Service ? COLOR_TAB_ACTIVE : COLOR_TAB_INACTIVE);
    FillRect(hdc, &state->tabServiceRect, serviceBrush);
    DeleteObject(serviceBrush);
    
    // Active indicator
    RECT indicator;
    if (state->currentTab == TabMode::Host) {
        indicator = {0, TAB_HEIGHT - 3, tabWidth, TAB_HEIGHT};
    } else if (state->currentTab == TabMode::Viewer) {
        indicator = {tabWidth, TAB_HEIGHT - 3, tabWidth * 2, TAB_HEIGHT};
    } else {
        indicator = {tabWidth * 2, TAB_HEIGHT - 3, width, TAB_HEIGHT};
    }
    HBRUSH accentBrush = CreateSolidBrush(COLOR_ACCENT);
    FillRect(hdc, &indicator, accentBrush);
    DeleteObject(accentBrush);
    
    // Tab text
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, g_fontBold);
    
    SetTextColor(hdc, state->currentTab == TabMode::Host ? COLOR_TEXT : COLOR_TEXT_DIM);
    DrawTextW(hdc, L"COMPARTIR", -1, &state->tabHostRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    
    SetTextColor(hdc, state->currentTab == TabMode::Viewer ? COLOR_TEXT : COLOR_TEXT_DIM);
    DrawTextW(hdc, L"VER", -1, &state->tabViewerRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    
    SetTextColor(hdc, state->currentTab == TabMode::Service ? COLOR_TEXT : COLOR_TEXT_DIM);
    DrawTextW(hdc, L"SERVICIO", -1, &state->tabServiceRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void updateTabVisibility(MainWindowState* state) {
    bool hostVisible = state->currentTab == TabMode::Host;
    bool viewerVisible = state->currentTab == TabMode::Viewer;
    bool serviceVisible = state->currentTab == TabMode::Service;
    
    // Host controls
    if (state->hostCodeEdit) ShowWindow(state->hostCodeEdit, hostVisible ? SW_SHOW : SW_HIDE);
    if (state->hostLocalIPLabel) ShowWindow(state->hostLocalIPLabel, hostVisible ? SW_SHOW : SW_HIDE);
    if (state->hostButton) ShowWindow(state->hostButton, hostVisible ? SW_SHOW : SW_HIDE);
    if (state->hostStatus) ShowWindow(state->hostStatus, hostVisible ? SW_SHOW : SW_HIDE);
    if (state->hostQualityCombo) ShowWindow(state->hostQualityCombo, hostVisible ? SW_SHOW : SW_HIDE);
    if (state->hostQualityLabel) ShowWindow(state->hostQualityLabel, hostVisible ? SW_SHOW : SW_HIDE);
    if (state->hostMetricsLabel) ShowWindow(state->hostMetricsLabel, hostVisible ? SW_SHOW : SW_HIDE);
    if (state->bannerStatic) ShowWindow(state->bannerStatic, hostVisible ? SW_SHOW : SW_HIDE);
    
    // Viewer controls
    if (state->viewerCodeEdit) ShowWindow(state->viewerCodeEdit, viewerVisible ? SW_SHOW : SW_HIDE);
    if (state->viewerButton) ShowWindow(state->viewerButton, viewerVisible ? SW_SHOW : SW_HIDE);
    if (state->viewerCanvas) ShowWindow(state->viewerCanvas, SW_HIDE); // Solo se muestra al conectar
    
    // Service controls
    if (state->serviceCodeLabel) ShowWindow(state->serviceCodeLabel, serviceVisible ? SW_SHOW : SW_HIDE);
    if (state->serviceCodeEdit) ShowWindow(state->serviceCodeEdit, serviceVisible ? SW_SHOW : SW_HIDE);
    if (state->serviceGenerateBtn) ShowWindow(state->serviceGenerateBtn, serviceVisible ? SW_SHOW : SW_HIDE);
    if (state->serviceAutoReconnect) ShowWindow(state->serviceAutoReconnect, serviceVisible ? SW_SHOW : SW_HIDE);
    if (state->serviceNoAutoCode) ShowWindow(state->serviceNoAutoCode, serviceVisible ? SW_SHOW : SW_HIDE);
    if (state->serviceInstallBtn) ShowWindow(state->serviceInstallBtn, serviceVisible ? SW_SHOW : SW_HIDE);
    if (state->serviceUninstallBtn) ShowWindow(state->serviceUninstallBtn, serviceVisible ? SW_SHOW : SW_HIDE);
    if (state->serviceStartBtn) ShowWindow(state->serviceStartBtn, serviceVisible ? SW_SHOW : SW_HIDE);
    if (state->serviceStopBtn) ShowWindow(state->serviceStopBtn, serviceVisible ? SW_SHOW : SW_HIDE);
    if (state->serviceStatus) ShowWindow(state->serviceStatus, serviceVisible ? SW_SHOW : SW_HIDE);
    if (state->serviceRefreshBtn) ShowWindow(state->serviceRefreshBtn, serviceVisible ? SW_SHOW : SW_HIDE);
}

// Service management functions
bool isServiceInstalled() {
    SC_HANDLE scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    
    SC_HANDLE service = OpenServiceW(scm, L"VicViewerService", SERVICE_QUERY_STATUS);
    bool installed = (service != nullptr);
    
    if (service) CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return installed;
}

bool isServiceRunning() {
    SC_HANDLE scm = OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    
    SC_HANDLE service = OpenServiceW(scm, L"VicViewerService", SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(scm);
        return false;
    }
    
    SERVICE_STATUS status{};
    bool running = QueryServiceStatus(service, &status) && status.dwCurrentState == SERVICE_RUNNING;
    
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    return running;
}

void updateServiceStatus(MainWindowState* state) {
    if (!state->serviceStatus) return;
    
    if (!isServiceInstalled()) {
        SetWindowTextW(state->serviceStatus, L"Estado: No instalado");
        EnableWindow(state->serviceInstallBtn, TRUE);
        EnableWindow(state->serviceUninstallBtn, FALSE);
        EnableWindow(state->serviceStartBtn, FALSE);
        EnableWindow(state->serviceStopBtn, FALSE);
    } else if (isServiceRunning()) {
        SetWindowTextW(state->serviceStatus, L"Estado: Ejecutando OK");
        EnableWindow(state->serviceInstallBtn, FALSE);
        EnableWindow(state->serviceUninstallBtn, FALSE);
        EnableWindow(state->serviceStartBtn, FALSE);
        EnableWindow(state->serviceStopBtn, TRUE);
    } else {
        SetWindowTextW(state->serviceStatus, L"Estado: Detenido");
        EnableWindow(state->serviceInstallBtn, FALSE);
        EnableWindow(state->serviceUninstallBtn, TRUE);
        EnableWindow(state->serviceStartBtn, TRUE);
        EnableWindow(state->serviceStopBtn, FALSE);
    }
}

// Lee solo la opcion NO_AUTO_CODE del archivo de config (para usar antes de crear UI)
bool readNoAutoCodeSetting() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring configPath(exePath);
    size_t lastSlash = configPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        configPath = configPath.substr(0, lastSlash + 1);
    }
    configPath += L"vicviewer_service.cfg";
    
    FILE* f = _wfopen(configPath.c_str(), L"r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "NO_AUTO_CODE=", 13) == 0) {
                int val = atoi(line + 13);
                fclose(f);
                return val != 0;
            }
        }
        fclose(f);
    }
    return false;  // Por defecto, generar codigo al inicio
}

void saveServiceConfig(MainWindowState* state) {
    wchar_t codeBuffer[64];
    GetWindowTextW(state->serviceCodeEdit, codeBuffer, 64);
    
    // Obtener estado de los checkboxes
    bool autoReconnect = (SendMessage(state->serviceAutoReconnect, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool noAutoCode = (SendMessage(state->serviceNoAutoCode, BM_GETCHECK, 0, 0) == BST_CHECKED);
    
    // Get executable directory
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring configPath(exePath);
    size_t lastSlash = configPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        configPath = configPath.substr(0, lastSlash + 1);
    }
    configPath += L"vicviewer_service.cfg";
    
    // Write config file
    FILE* f = _wfopen(configPath.c_str(), L"w");
    if (f) {
        fprintf(f, "# Configuracion del servicio VicViewer\n");
        fprintf(f, "CODE=%ls\n", codeBuffer);
        fprintf(f, "AUTO_RECONNECT=%d\n", autoReconnect ? 1 : 0);
        fprintf(f, "NO_AUTO_CODE=%d\n", noAutoCode ? 1 : 0);
        fclose(f);
        
        vic::logging::global().log(vic::logging::Logger::Level::Info,
            std::string("Config guardada - Codigo: ") + wideToUtf8(codeBuffer) + 
            ", AutoReconnect: " + (autoReconnect ? "SI" : "NO") +
            ", NoAutoCode: " + (noAutoCode ? "SI" : "NO"));
    }
}

void loadServiceConfig(MainWindowState* state) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring configPath(exePath);
    size_t lastSlash = configPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        configPath = configPath.substr(0, lastSlash + 1);
    }
    configPath += L"vicviewer_service.cfg";
    
    FILE* f = _wfopen(configPath.c_str(), L"r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "CODE=", 5) == 0) {
                char* code = line + 5;
                // Remove newline
                char* nl = strchr(code, '\n');
                if (nl) *nl = '\0';
                nl = strchr(code, '\r');
                if (nl) *nl = '\0';
                
                SetWindowTextA(state->serviceCodeEdit, code);
            }
            else if (strncmp(line, "AUTO_RECONNECT=", 15) == 0) {
                int autoReconnect = atoi(line + 15);
                SendMessage(state->serviceAutoReconnect, BM_SETCHECK, 
                    autoReconnect ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            else if (strncmp(line, "NO_AUTO_CODE=", 13) == 0) {
                int noAutoCode = atoi(line + 13);
                SendMessage(state->serviceNoAutoCode, BM_SETCHECK, 
                    noAutoCode ? BST_CHECKED : BST_UNCHECKED, 0);
            }
        }
        fclose(f);
    }
}

void runServiceCommand(const wchar_t* args, HWND hwnd) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring servicePath(exePath);
    size_t lastSlash = servicePath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        servicePath = servicePath.substr(0, lastSlash + 1);
    }
    servicePath += L"VicViewerService.exe";
    
    // Check if service exe exists
    if (GetFileAttributesW(servicePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hwnd, L"No se encontró VicViewerService.exe\nAsegúrate de que está en la misma carpeta que VicViewer.exe", L"Error", MB_ICONERROR);
        return;
    }
    
    std::wstring cmd = servicePath + L" " + args;
    
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";  // Run as admin
    sei.lpFile = servicePath.c_str();
    sei.lpParameters = args;
    sei.hwnd = hwnd;
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    
    if (ShellExecuteExW(&sei)) {
        // Wait for process to complete
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, 5000);
            CloseHandle(sei.hProcess);
        }
    }
}

// Host functions
void startHost(MainWindowState* state) {
    if (!state || state->hostRunning) return;

    // Configurar companyCode y diskSerial en el matchmaker
    if (!state->companyCode.empty()) {
        state->matchmaker->setCompanyCode(state->companyCode);
        vic::logging::global().log(vic::logging::Logger::Level::Info,
            "[UI] CompanyCode configurado: " + state->companyCode);
    }
    if (!state->diskSerial.empty()) {
        state->matchmaker->setDiskSerial(state->diskSerial);
    }
    // Legacy: también setear clientId
    if (!state->clientId.empty()) {
        state->matchmaker->setClientId(state->clientId);
    }

    // Configurar código fijo si está definido
    if (!state->fixedCode.empty()) {
        state->hostSession->setFixedCode(state->fixedCode);
        vic::logging::global().log(vic::logging::Logger::Level::Info,
            "[UI] Usando codigo fijo: " + state->fixedCode);
    }

    // Aplicar configuración de calidad desde el ComboBox
    int qualityIndex = static_cast<int>(SendMessage(state->hostQualityCombo, CB_GETCURSEL, 0, 0));
    vic::pipeline::StreamConfig config;
    switch (qualityIndex) {
        case 0:  // Bajo
            config.applyPreset(vic::pipeline::QualityPreset::Low);
            vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] Calidad: Bajo (540p, 1000kbps)");
            break;
        case 2:  // Alto
            config.applyPreset(vic::pipeline::QualityPreset::High);
            vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] Calidad: Alto (1080p, 4000kbps)");
            break;
        default:  // Medio (predeterminado)
            config.applyPreset(vic::pipeline::QualityPreset::Medium);
            vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] Calidad: Medio (720p, 2000kbps)");
            break;
    }
    state->hostSession->setStreamConfig(config);

    if (!state->hostSession->start()) {
        SetWindowTextW(state->hostStatus, L"Error al iniciar");
        return;
    }

    state->hostRunning = true;
    SetWindowTextW(state->hostButton, L"Detener");
    // Deshabilitar combo durante streaming
    EnableWindow(state->hostQualityCombo, FALSE);

    if (auto info = state->hostSession->connectionInfo()) {
        SetWindowTextW(state->hostStatus, L"Registrando...");
        
        // Host siempre es GRATIS - quien paga es el Viewer
        if (auto result = state->matchmaker->registerHostExtended(*info)) {
            auto code = utf8ToWide(result->code);
            SetWindowTextW(state->hostCodeEdit, code.c_str());
            
            if (result->emailSent) {
                SetWindowTextW(state->hostStatus, L"Codigo enviado por email");
            } else {
                SetWindowTextW(state->hostStatus, L"Comparte este codigo");
            }
            
            // Guardar código activo para heartbeat
            state->activeCode = result->code;
            
            // Actualizar banner
            updateBanner(state, !state->companyCode.empty());
            
            // Iniciar heartbeat timer (cada 60 segundos)
            SetTimer(state->mainWindow, 2, 60000, nullptr);
        } else {
            auto code = utf8ToWide(info->code);
            SetWindowTextW(state->hostCodeEdit, code.c_str());
            SetWindowTextW(state->hostStatus, L"Reintentando registro...");
        }
    }

    addTrayIcon(state->mainWindow, state, state->isFreeMode ? L"Vicviewer - Modo Gratuito" : L"Vicviewer - Compartiendo");
    
    // Iniciar timer para detectar cuando un viewer conecta y actualizar métricas
    SetTimer(state->mainWindow, 1, 500, nullptr);  // Más frecuente para métricas
}

void stopHost(MainWindowState* state) {
    if (!state || !state->hostRunning) return;
    
    // Si estaba en modo FREE, registrar fin de sesión
    if (state->isFreeMode) {
        state->matchmaker->endFreeSession();
        if (state->freeSessionTimer != 0) {
            KillTimer(state->mainWindow, 3);  // Detener timer de sesión FREE
            state->freeSessionTimer = 0;
        }
        state->isFreeMode = false;
        vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] Sesion FREE terminada");
    }
    
    // Enviar desconexión limpia al servidor
    if (!state->activeCode.empty()) {
        state->matchmaker->disconnect(state->activeCode);
        KillTimer(state->mainWindow, 2);  // Detener heartbeat
        state->activeCode.clear();
    }
    
    state->hostSession->stop();
    state->hostRunning = false;
    state->hiddenToTrayOnce = false;  // Resetear flag para próxima sesión
    SetWindowTextW(state->hostButton, L"Compartir");
    SetWindowTextW(state->hostCodeEdit, L"");
    SetWindowTextW(state->hostStatus, L"");
    SetWindowTextW(state->hostMetricsLabel, L"");
    EnableWindow(state->hostQualityCombo, TRUE);  // Rehabilitar combo
    removeTrayIcon(state);
}

// Helper para verificar suscripción y validar contraseña de servicio
// Retorna true si la validación fue exitosa
bool validateServiceSubscription(MainWindowState* state, HWND parentHwnd) {
    if (!state) return false;
    
    // Verificar si tiene companyCode (cuenta empresarial)
    if (state->companyCode.empty()) {
        MessageBoxW(parentHwnd, 
            L"Esta funcion requiere una suscripcion activa.\n\n"
            L"Para obtener acceso a funciones de servicio (codigo fijo,\n"
            L"instalacion como servicio de Windows), adquiera una suscripcion\n"
            L"en vicviewer.com", 
            L"Suscripcion Requerida", MB_ICONINFORMATION);
        return false;
    }
    
    // IMPORTANTE: Configurar companyCode en matchmaker ANTES de validar
    state->matchmaker->setCompanyCode(state->companyCode);
    if (!state->clientId.empty()) {
        state->matchmaker->setClientId(state->clientId);
    }
    
    // Registrar clase de ventana para el dialogo
    static bool classRegistered = false;
    const wchar_t* dlgClassName = L"VicViewerServicePwdDlg";
    if (!classRegistered) {
        WNDCLASSEXW wcDlg = {};
        wcDlg.cbSize = sizeof(wcDlg);
        wcDlg.lpfnWndProc = PasswordDlgProc;
        wcDlg.hInstance = GetModuleHandle(nullptr);
        wcDlg.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcDlg.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wcDlg.lpszClassName = dlgClassName;
        RegisterClassExW(&wcDlg);
        classRegistered = true;
    }
    
    g_pwdOkClicked = false;
    g_pwdBuffer[0] = L'\0';
    
    // Centrar en pantalla
    int dlgW = 360, dlgH = 140;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - dlgW) / 2;
    int posY = (screenH - dlgH) / 2;
    
    // Crear ventana del dialogo
    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        dlgClassName, L"Acceso empresarial",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        posX, posY, dlgW, dlgH,
        parentHwnd, nullptr, GetModuleHandle(nullptr), nullptr);
    
    if (!hDlg) {
        MessageBoxW(parentHwnd, L"Error al crear dialogo.", L"Error", MB_ICONERROR);
        return false;
    }
    
    // Texto de instruccion
    HWND hLabel2 = CreateWindowW(L"STATIC", L"Ingrese su clave de servicio:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        10, 15, 330, 20, hDlg, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessage(hLabel2, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    
    // Campo de texto
    g_pwdEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_UPPERCASE,
        10, 40, 220, 30, hDlg, (HMENU)100, GetModuleHandle(nullptr), nullptr);
    SendMessage(g_pwdEdit, EM_SETLIMITTEXT, 5, 0);
    SendMessage(g_pwdEdit, WM_SETFONT, (WPARAM)g_fontCode, TRUE);
    
    // Boton Validar
    HWND hBtnOk = CreateWindowW(L"BUTTON", L"Validar",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        240, 40, 100, 30, hDlg, (HMENU)IDOK, GetModuleHandle(nullptr), nullptr);
    SendMessage(hBtnOk, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    
    // Texto de ayuda
    HWND hLabel3 = CreateWindowW(L"STATIC", L"Obtenga su clave en vicviewer.com",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        10, 80, 220, 20, hDlg, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessage(hLabel3, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    
    // Boton Cancelar
    HWND hBtnCancel = CreateWindowW(L"BUTTON", L"Cancelar",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        240, 80, 100, 28, hDlg, (HMENU)IDCANCEL, GetModuleHandle(nullptr), nullptr);
    SendMessage(hBtnCancel, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    
    SetFocus(g_pwdEdit);
    
    // Message loop
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessage(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    
    DestroyWindow(hDlg);
    
    if (!g_pwdOkClicked) {
        return false;  // Usuario cancelo
    }
    
    // Validar contrasena con el servidor
    std::string pwd = wideToUtf8(g_pwdBuffer);
    auto result = state->matchmaker->validateServicePassword(pwd);
    if (result && result->valid) {
        return true;
    } else {
        std::wstring errMsg = L"Clave incorrecta.";
        if (result && !result->message.empty()) {
            errMsg = utf8ToWide(result->message);
        }
        MessageBoxW(parentHwnd, errMsg.c_str(), L"Error", MB_ICONWARNING);
        return false;
    }
}

// =========== LAN DIRECT CONNECTION HELPER ===========
// Detecta si el input es una dirección IP (formato x.x.x.x)
bool isIPAddress(const std::string& input) {
    // IP válida: 4 octetos separados por puntos, cada octeto 0-255
    int dots = 0;
    int octets = 0;
    std::string currentOctet;
    
    for (char c : input) {
        if (c == '.') {
            if (currentOctet.empty() || currentOctet.length() > 3) return false;
            int val = std::stoi(currentOctet);
            if (val < 0 || val > 255) return false;
            octets++;
            currentOctet.clear();
            dots++;
            if (dots > 3) return false;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            currentOctet += c;
            if (currentOctet.length() > 3) return false;
        } else {
            return false;  // Carácter inválido
        }
    }
    
    // Validar último octeto
    if (currentOctet.empty() || currentOctet.length() > 3) return false;
    int val = std::stoi(currentOctet);
    if (val < 0 || val > 255) return false;
    octets++;
    
    return (octets == 4 && dots == 3);
}

// Viewer functions
void connectViewer(MainWindowState* state) {
    if (!state) return;
    
    vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] connectViewer: iniciando conexion");
    
    // Si NO hay companyCode (modo FREE), verificar tiempo de espera anti-abuso
    if (state->companyCode.empty()) {
        auto& antiAbuse = AntiAbuse::instance();
        if (!antiAbuse.canStartFreeSession()) {
            std::wstring msg = antiAbuse.getWaitMessage();
            MessageBoxW(state->mainWindow, msg.c_str(), 
                L"Por favor espere", MB_ICONINFORMATION);
            return;
        }
    }
    
    // Si hay companyCode, validar clave de servicio antes de conectar
    if (!state->companyCode.empty() && !state->servicePasswordValidated) {
        if (!validateServicePasswordOnStartup(state->companyCode)) {
            vic::logging::global().log(vic::logging::Logger::Level::Warning, 
                "[UI] connectViewer: validacion de clave cancelada o fallida");
            return;
        }
        state->servicePasswordValidated = true;
    }

    wchar_t buffer[32];
    GetWindowTextW(state->viewerCodeEdit, buffer, 32);
    std::wstring code(buffer);
    
    if (code.empty()) {
        vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] connectViewer: codigo vacio, saliendo");
        return;
    }

    std::string inputUtf8 = wideToUtf8(code);
    
    // Detectar si es IP o código
    bool isDirectIP = isIPAddress(inputUtf8);
    vic::logging::global().log(vic::logging::Logger::Level::Info, 
        "[UI] connectViewer: input=" + inputUtf8 + ", isIP=" + (isDirectIP ? "true" : "false"));
    
    // Para conexión por código, resolver primero
    if (!isDirectIP) {
        vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] connectViewer: resolviendo codigo " + inputUtf8);
        auto info = state->matchmaker->resolveCode(inputUtf8);
        if (!info) {
            vic::logging::global().log(vic::logging::Logger::Level::Warning, "[UI] connectViewer: codigo no encontrado");
            SetWindowTextW(state->viewerButton, L"No encontrado");
            EnableWindow(state->viewerButton, TRUE);
            return;
        }
        vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] connectViewer: codigo resuelto OK");
    }

    // Configurar callback de frames (igual para ambos modos)
    state->viewerSession->setFrameCallback([state](const vic::capture::DesktopFrame& frame) {
        vic::logging::global().log(vic::logging::Logger::Level::Info, 
            "[UI] FRAME RECIBIDO: " + std::to_string(frame.width) + "x" + std::to_string(frame.height));
        
        std::lock_guard lock(state->frameMutex);
        state->lastFrame = frame;
        
        if (!state->viewerConnected) {
            state->viewerConnected = true;
            // Cancelar timer de timeout - conexión exitosa
            KillTimer(state->mainWindow, TIMER_VIEWER_CONNECT_TIMEOUT);
            vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] Primer frame! Cancelando timeout y enviando WM_VIEWER_CONNECTED");
            PostMessage(state->mainWindow, WM_VIEWER_CONNECTED, 0, 0);
        }
        
        if (state->viewerCanvas) {
            InvalidateRect(state->viewerCanvas, nullptr, FALSE);
        }
    });

    SetWindowTextW(state->viewerButton, L"Conectando...");
    
    // Iniciar timer de timeout
    SetTimer(state->mainWindow, TIMER_VIEWER_CONNECT_TIMEOUT, VIEWER_CONNECT_TIMEOUT_MS, nullptr);
    vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] connectViewer: Timer de timeout iniciado (90s)");
    
    bool connectResult = false;
    
    if (isDirectIP) {
        // Conexión LAN directa por IP
        vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] connectViewer: conexion LAN directa a " + inputUtf8);
        connectResult = state->viewerSession->connectDirect(inputUtf8);
        vic::logging::global().log(vic::logging::Logger::Level::Info, 
            "[UI] connectViewer: connectDirect() retorno " + std::string(connectResult ? "true" : "false"));
    } else {
        // Conexión por código (matchmaker)
        vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] connectViewer: llamando viewerSession->connect()");
        connectResult = state->viewerSession->connect(inputUtf8);
        vic::logging::global().log(vic::logging::Logger::Level::Info, 
            "[UI] connectViewer: connect() retorno " + std::string(connectResult ? "true" : "false"));
        
        vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] connectViewer: habilitando auto-reconnect");
        state->viewerSession->enableAutoReconnect(inputUtf8);
    }
    
    vic::logging::global().log(vic::logging::Logger::Level::Info, "[UI] connectViewer: completado");
}

// Window procedures
LRESULT CALLBACK ViewerCanvasProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

void createControls(MainWindowState* state, HWND parent) {
    int contentY = TAB_HEIGHT + MARGIN;
    int contentWidth = WINDOW_WIDTH - MARGIN * 2;
    
    // === HOST CONTROLS ===
    // Quality label
    state->hostQualityLabel = CreateWindowW(L"STATIC", L"Calidad:",
        WS_CHILD | WS_VISIBLE,
        MARGIN, contentY, 60, 20,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessage(state->hostQualityLabel, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    
    // Quality combo box
    state->hostQualityCombo = CreateWindowW(L"COMBOBOX", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
        MARGIN + 65, contentY - 3, 150, 100,
        parent, reinterpret_cast<HMENU>(1005), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->hostQualityCombo, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    SendMessage(state->hostQualityCombo, CB_ADDSTRING, 0, (LPARAM)L"Bajo (540p)");
    SendMessage(state->hostQualityCombo, CB_ADDSTRING, 0, (LPARAM)L"Medio (720p)");
    SendMessage(state->hostQualityCombo, CB_ADDSTRING, 0, (LPARAM)L"Alto (1080p)");
    SendMessage(state->hostQualityCombo, CB_SETCURSEL, 1, 0);  // Default: Medio
    
    // Code display (read-only, large)
    state->hostCodeEdit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_READONLY | ES_CENTER,
        MARGIN, contentY + 35, contentWidth, 40,
        parent, reinterpret_cast<HMENU>(1001), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->hostCodeEdit, WM_SETFONT, (WPARAM)g_fontCode, TRUE);
    
    // Local IP label for LAN connections
    std::string localIP = getLocalIPAddress();
    std::wstring ipText = L"IP Local: " + utf8ToWide(localIP);
    state->hostLocalIPLabel = CreateWindowW(L"STATIC", ipText.c_str(),
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        MARGIN, contentY + 78, contentWidth, 16,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessage(state->hostLocalIPLabel, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    
    // Start button
    state->hostButton = CreateWindowW(L"BUTTON", L"Compartir",
        WS_CHILD | WS_VISIBLE | BS_FLAT,
        MARGIN, contentY + 98, contentWidth, 36,
        parent, reinterpret_cast<HMENU>(1002), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->hostButton, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    
    // Status text
    state->hostStatus = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        MARGIN, contentY + 148, contentWidth, 20,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessage(state->hostStatus, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    
    // Metrics display (FPS, bitrate)
    state->hostMetricsLabel = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        MARGIN, contentY + 173, contentWidth, 20,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessage(state->hostMetricsLabel, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);

    // Banner (460x60) - se muestra en la parte inferior
    state->bannerStatic = CreateWindowW(L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_CENTERIMAGE,
        MARGIN, contentY + 203, BANNER_WIDTH, BANNER_HEIGHT,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);

    // === VIEWER CONTROLS ===
    // Code input (también acepta IP para conexión LAN directa)
    state->viewerCodeEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_CENTER,
        MARGIN, contentY + 30, contentWidth, 40,
        parent, reinterpret_cast<HMENU>(2001), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->viewerCodeEdit, WM_SETFONT, (WPARAM)g_fontCode, TRUE);
    SendMessage(state->viewerCodeEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Codigo o IP");
    
    // Connect button
    state->viewerButton = CreateWindowW(L"BUTTON", L"Conectar",
        WS_CHILD | BS_FLAT,
        MARGIN, contentY + 90, contentWidth, 36,
        parent, reinterpret_cast<HMENU>(2002), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->viewerButton, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    
    // Canvas para mostrar escritorio remoto (oculto hasta conectar)
    // Usar WS_TABSTOP para poder recibir foco de teclado
    state->viewerCanvas = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_TABSTOP | SS_NOTIFY,  // SS_NOTIFY permite clics, WS_TABSTOP permite foco
        -100, -100, 1, 1,  // Fuera de pantalla hasta conectar
        parent, reinterpret_cast<HMENU>(2004), GetModuleHandle(nullptr), nullptr);
    SetWindowLongPtr(state->viewerCanvas, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    g_originalCanvasProc = reinterpret_cast<WNDPROC>(SetWindowLongPtr(state->viewerCanvas, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(ViewerCanvasProc)));
    
    // === SERVICE CONTROLS ===
    int btnWidth = (contentWidth - 10) / 2;
    int codeInputWidth = contentWidth - 90;  // Espacio para botón Generar
    
    // Label for code
    state->serviceCodeLabel = CreateWindowW(L"STATIC", L"Codigo fijo para acceso remoto:",
        WS_CHILD | SS_LEFT,
        MARGIN, contentY + 10, contentWidth, 20,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessage(state->serviceCodeLabel, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    
    // Code input (más pequeño para dejar espacio al botón Generar)
    state->serviceCodeEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | ES_CENTER | ES_UPPERCASE,
        MARGIN, contentY + 35, codeInputWidth, 35,
        parent, reinterpret_cast<HMENU>(3001), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->serviceCodeEdit, WM_SETFONT, (WPARAM)g_fontCode, TRUE);
    SendMessage(state->serviceCodeEdit, EM_SETLIMITTEXT, 12, 0);
    
    // Botón Generar código
    state->serviceGenerateBtn = CreateWindowW(L"BUTTON", L"Generar",
        WS_CHILD | BS_FLAT,
        MARGIN + codeInputWidth + 5, contentY + 35, 80, 35,
        parent, reinterpret_cast<HMENU>(3007), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->serviceGenerateBtn, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    
    // Checkbox para reconectar al reiniciar (deshabilitado hasta validar clave)
    state->serviceAutoReconnect = CreateWindowW(L"BUTTON", L"Reconectar automaticamente al reiniciar Windows",
        WS_CHILD | BS_AUTOCHECKBOX | WS_DISABLED,
        MARGIN, contentY + 78, contentWidth, 22,
        parent, reinterpret_cast<HMENU>(3008), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->serviceAutoReconnect, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    SendMessage(state->serviceAutoReconnect, BM_SETCHECK, BST_CHECKED, 0);  // Marcado por defecto
    
    // Checkbox para no generar codigo al inicio (deshabilitado hasta validar clave)
    state->serviceNoAutoCode = CreateWindowW(L"BUTTON", L"No generar codigo automaticamente al iniciar",
        WS_CHILD | BS_AUTOCHECKBOX | WS_DISABLED,
        MARGIN, contentY + 100, contentWidth, 22,
        parent, reinterpret_cast<HMENU>(3009), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->serviceNoAutoCode, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    // Desmarcado por defecto (se genera codigo al inicio)
    
    // Install / Uninstall buttons row
    state->serviceInstallBtn = CreateWindowW(L"BUTTON", L"Instalar Servicio",
        WS_CHILD | BS_FLAT,
        MARGIN, contentY + 132, btnWidth, 36,
        parent, reinterpret_cast<HMENU>(3002), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->serviceInstallBtn, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    
    state->serviceUninstallBtn = CreateWindowW(L"BUTTON", L"Desinstalar",
        WS_CHILD | BS_FLAT,
        MARGIN + btnWidth + 10, contentY + 132, btnWidth, 36,
        parent, reinterpret_cast<HMENU>(3003), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->serviceUninstallBtn, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    
    // Start / Stop buttons row
    state->serviceStartBtn = CreateWindowW(L"BUTTON", L"Iniciar",
        WS_CHILD | BS_FLAT,
        MARGIN, contentY + 177, btnWidth, 36,
        parent, reinterpret_cast<HMENU>(3004), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->serviceStartBtn, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    
    state->serviceStopBtn = CreateWindowW(L"BUTTON", L"Detener",
        WS_CHILD | BS_FLAT,
        MARGIN + btnWidth + 10, contentY + 177, btnWidth, 36,
        parent, reinterpret_cast<HMENU>(3005), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->serviceStopBtn, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
    
    // Status label
    state->serviceStatus = CreateWindowW(L"STATIC", L"Estado: Verificando...",
        WS_CHILD | SS_CENTER,
        MARGIN, contentY + 222, contentWidth - 90, 20,
        parent, nullptr, GetModuleHandle(nullptr), nullptr);
    SendMessage(state->serviceStatus, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    
    // Refresh button
    state->serviceRefreshBtn = CreateWindowW(L"BUTTON", L"Actualizar",
        WS_CHILD | BS_FLAT,
        MARGIN + contentWidth - 80, contentY + 217, 80, 30,
        parent, reinterpret_cast<HMENU>(3006), GetModuleHandle(nullptr), nullptr);
    SendMessage(state->serviceRefreshBtn, WM_SETFONT, (WPARAM)g_fontNormal, TRUE);
    
    // Load config and update status
    loadServiceConfig(state);
    updateServiceStatus(state);
    
    // Cargar banner por defecto (se actualizara cuando se genere codigo)
    updateBanner(state, false);  // false = modo free, usa banner por defecto
    
    updateTabVisibility(state);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = getWindowState(hwnd);

    switch (msg) {
    case WM_CREATE: {
        auto cs = reinterpret_cast<LPCREATESTRUCT>(lParam);
        state = reinterpret_cast<MainWindowState*>(cs->lpCreateParams);
        setWindowUserData(hwnd, state);
        state->mainWindow = hwnd;
        
        initGdiResources();
        createControls(state, hwnd);
        
        // Dark title bar (Windows 10+)
        BOOL darkMode = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode));
        
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        
        RECT rect;
        GetClientRect(hwnd, &rect);
        FillRect(hdc, &rect, g_bgBrush);
        
        if (state) {
            drawTabs(hdc, state, rect.right);
        }
        
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: {
        HDC hdcCtrl = (HDC)wParam;
        SetTextColor(hdcCtrl, COLOR_TEXT);
        SetBkColor(hdcCtrl, COLOR_BG_LIGHT);
        return (LRESULT)g_bgLightBrush;
    }
    case WM_CTLCOLORBTN: {
        return (LRESULT)g_bgLightBrush;
    }
    case WM_LBUTTONDOWN: {
        if (!state) break;
        
        // Si estamos conectados como viewer, enviar evento de mouse
        if (state->viewerConnected && state->viewerSession->isConnected()) {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT rect;
            GetClientRect(hwnd, &rect);
            
            std::lock_guard lock(state->frameMutex);
            if (state->lastFrame) {
                float fx = static_cast<float>(pt.x) / rect.right;
                float fy = static_cast<float>(pt.y) / rect.bottom;
                
                // Usar originalWidth/Height para coordenadas correctas (compensar escalado)
                uint32_t targetW = state->lastFrame->originalWidth > 0 ? state->lastFrame->originalWidth : state->lastFrame->width;
                uint32_t targetH = state->lastFrame->originalHeight > 0 ? state->lastFrame->originalHeight : state->lastFrame->height;
                
                vic::input::MouseEvent ev{};
                ev.absolute = true;
                ev.x = static_cast<int32_t>(fx * targetW);
                ev.y = static_cast<int32_t>(fy * targetH);
                ev.action = vic::input::MouseAction::Down;
                ev.button = vic::input::MouseButton::Left;
                
                vic::logging::global().log(vic::logging::Logger::Level::Info,
                    "[UI-Main] LBUTTONDOWN: x=" + std::to_string(ev.x) + " y=" + std::to_string(ev.y));
                state->viewerSession->sendMouseEvent(ev);
            }
            break;
        }
        
        // Manejo de tabs
        POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        
        if (PtInRect(&state->tabHostRect, pt) && state->currentTab != TabMode::Host) {
            state->currentTab = TabMode::Host;
            updateTabVisibility(state);
            InvalidateRect(hwnd, nullptr, TRUE);
        } else if (PtInRect(&state->tabViewerRect, pt) && state->currentTab != TabMode::Viewer) {
            state->currentTab = TabMode::Viewer;
            updateTabVisibility(state);
            InvalidateRect(hwnd, nullptr, TRUE);
        } else if (PtInRect(&state->tabServiceRect, pt) && state->currentTab != TabMode::Service) {
            state->currentTab = TabMode::Service;
            // Cargar configuracion guardada al entrar a la pestana Servicio
            loadServiceConfig(state);
            // Habilitar checkboxes solo si se valido la clave (o no hay companyCode)
            bool enableCheckboxes = state->companyCode.empty() || state->servicePasswordValidated;
            if (state->serviceAutoReconnect) {
                EnableWindow(state->serviceAutoReconnect, enableCheckboxes ? TRUE : FALSE);
                ShowWindow(state->serviceAutoReconnect, SW_SHOW);
            }
            if (state->serviceNoAutoCode) {
                // Este checkbox solo para suscriptores pagados
                EnableWindow(state->serviceNoAutoCode, (enableCheckboxes && !state->isFreeMode) ? TRUE : FALSE);
                ShowWindow(state->serviceNoAutoCode, SW_SHOW);
                if (state->isFreeMode) {
                    SendMessage(state->serviceNoAutoCode, BM_SETCHECK, BST_UNCHECKED, 0);
                }
            }
            updateTabVisibility(state);
            updateServiceStatus(state);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        break;
    }
    case WM_LBUTTONUP: {
        if (!state) break;
        if (state->viewerConnected && state->viewerSession->isConnected()) {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT rect;
            GetClientRect(hwnd, &rect);
            
            std::lock_guard lock(state->frameMutex);
            if (state->lastFrame) {
                float fx = static_cast<float>(pt.x) / rect.right;
                float fy = static_cast<float>(pt.y) / rect.bottom;
                
                uint32_t targetW = state->lastFrame->originalWidth > 0 ? state->lastFrame->originalWidth : state->lastFrame->width;
                uint32_t targetH = state->lastFrame->originalHeight > 0 ? state->lastFrame->originalHeight : state->lastFrame->height;
                
                vic::input::MouseEvent ev{};
                ev.absolute = true;
                ev.x = static_cast<int32_t>(fx * targetW);
                ev.y = static_cast<int32_t>(fy * targetH);
                ev.action = vic::input::MouseAction::Up;
                ev.button = vic::input::MouseButton::Left;
                
                state->viewerSession->sendMouseEvent(ev);
            }
        }
        break;
    }
    case WM_RBUTTONDOWN: {
        if (!state) break;
        if (state->viewerConnected && state->viewerSession->isConnected()) {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT rect;
            GetClientRect(hwnd, &rect);
            
            std::lock_guard lock(state->frameMutex);
            if (state->lastFrame) {
                float fx = static_cast<float>(pt.x) / rect.right;
                float fy = static_cast<float>(pt.y) / rect.bottom;
                
                uint32_t targetW = state->lastFrame->originalWidth > 0 ? state->lastFrame->originalWidth : state->lastFrame->width;
                uint32_t targetH = state->lastFrame->originalHeight > 0 ? state->lastFrame->originalHeight : state->lastFrame->height;
                
                vic::input::MouseEvent ev{};
                ev.absolute = true;
                ev.x = static_cast<int32_t>(fx * targetW);
                ev.y = static_cast<int32_t>(fy * targetH);
                ev.action = vic::input::MouseAction::Down;
                ev.button = vic::input::MouseButton::Right;
                
                state->viewerSession->sendMouseEvent(ev);
            }
        }
        break;
    }
    case WM_RBUTTONUP: {
        if (!state) break;
        if (state->viewerConnected && state->viewerSession->isConnected()) {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT rect;
            GetClientRect(hwnd, &rect);
            
            std::lock_guard lock(state->frameMutex);
            if (state->lastFrame) {
                float fx = static_cast<float>(pt.x) / rect.right;
                float fy = static_cast<float>(pt.y) / rect.bottom;
                
                uint32_t targetW = state->lastFrame->originalWidth > 0 ? state->lastFrame->originalWidth : state->lastFrame->width;
                uint32_t targetH = state->lastFrame->originalHeight > 0 ? state->lastFrame->originalHeight : state->lastFrame->height;
                
                vic::input::MouseEvent ev{};
                ev.absolute = true;
                ev.x = static_cast<int32_t>(fx * targetW);
                ev.y = static_cast<int32_t>(fy * targetH);
                ev.action = vic::input::MouseAction::Up;
                ev.button = vic::input::MouseButton::Right;
                
                state->viewerSession->sendMouseEvent(ev);
            }
        }
        break;
    }
    case WM_MOUSEMOVE: {
        if (!state) break;
        if (state->viewerConnected && state->viewerSession->isConnected()) {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT rect;
            GetClientRect(hwnd, &rect);
            
            std::lock_guard lock(state->frameMutex);
            if (state->lastFrame) {
                float fx = static_cast<float>(pt.x) / rect.right;
                float fy = static_cast<float>(pt.y) / rect.bottom;
                
                uint32_t targetW = state->lastFrame->originalWidth > 0 ? state->lastFrame->originalWidth : state->lastFrame->width;
                uint32_t targetH = state->lastFrame->originalHeight > 0 ? state->lastFrame->originalHeight : state->lastFrame->height;
                
                vic::input::MouseEvent ev{};
                ev.absolute = true;
                ev.x = static_cast<int32_t>(fx * targetW);
                ev.y = static_cast<int32_t>(fy * targetH);
                ev.action = vic::input::MouseAction::Move;
                
                state->viewerSession->sendMouseEvent(ev);
            }
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        if (!state) break;
        if (state->viewerConnected && state->viewerSession->isConnected()) {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            RECT rect;
            GetClientRect(hwnd, &rect);
            
            std::lock_guard lock(state->frameMutex);
            if (state->lastFrame) {
                float fx = static_cast<float>(pt.x) / rect.right;
                float fy = static_cast<float>(pt.y) / rect.bottom;
                
                uint32_t targetW = state->lastFrame->originalWidth > 0 ? state->lastFrame->originalWidth : state->lastFrame->width;
                uint32_t targetH = state->lastFrame->originalHeight > 0 ? state->lastFrame->originalHeight : state->lastFrame->height;
                
                vic::input::MouseEvent ev{};
                ev.absolute = true;
                ev.x = static_cast<int32_t>(fx * targetW);
                ev.y = static_cast<int32_t>(fy * targetH);
                ev.action = vic::input::MouseAction::Wheel;
                ev.wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                
                state->viewerSession->sendMouseEvent(ev);
            }
        }
        break;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case 1002: // Host button
            if (state->hostRunning) {
                stopHost(state);
            } else {
                startHost(state);
            }
            break;
        case 2002: // Viewer button
            EnableWindow(state->viewerButton, FALSE);
            connectViewer(state);
            break;
        case 3002: { // Install service
            // Validar suscripción antes de permitir instalación
            if (!validateServiceSubscription(state, hwnd)) {
                break;
            }
            
            // Obtener código del campo
            char codeBuffer[32];
            GetWindowTextA(state->serviceCodeEdit, codeBuffer, sizeof(codeBuffer));
            std::string code(codeBuffer);
            
            // Si está vacío, generar uno
            if (code.empty()) {
                auto generated = state->matchmaker->generateAvailableCode();
                if (generated) {
                    code = *generated;
                    SetWindowTextA(state->serviceCodeEdit, code.c_str());
                } else {
                    MessageBoxW(hwnd, L"No se pudo generar un código. Verifica tu conexión a internet.", L"Error", MB_ICONWARNING);
                    break;
                }
            } else {
                // Verificar disponibilidad del código
                if (!state->matchmaker->checkCodeAvailability(code)) {
                    int result = MessageBoxW(hwnd, 
                        L"Este código ya está en uso.\n\n¿Deseas generar un código nuevo automáticamente?", 
                        L"Código no disponible", MB_YESNO | MB_ICONQUESTION);
                    if (result == IDYES) {
                        auto generated = state->matchmaker->generateAvailableCode();
                        if (generated) {
                            code = *generated;
                            SetWindowTextA(state->serviceCodeEdit, code.c_str());
                        } else {
                            MessageBoxW(hwnd, L"No se pudo generar un código.", L"Error", MB_ICONWARNING);
                            break;
                        }
                    } else {
                        break;  // Usuario canceló
                    }
                }
            }
            
            // Pre-registrar dispositivo en el servidor ANTES de instalar
            vic::logging::global().log(vic::logging::Logger::Level::Info,
                "Pre-registrando dispositivo con codigo: " + code);
            if (!state->matchmaker->preRegisterDevice(code)) {
                MessageBoxW(hwnd, L"No se pudo registrar el dispositivo en el servidor.\nVerifica tu conexión a internet.", L"Error", MB_ICONWARNING);
                break;
            }
            
            saveServiceConfig(state);
            runServiceCommand(L"--install", hwnd);
            Sleep(500);
            runServiceCommand(L"--start", hwnd);
            Sleep(500);
            updateServiceStatus(state);
            MessageBoxW(hwnd, L"Servicio instalado y dispositivo registrado correctamente.", L"Éxito", MB_ICONINFORMATION);
            break;
        }
        case 3003: // Uninstall service
            runServiceCommand(L"--stop", hwnd);
            Sleep(500);
            runServiceCommand(L"--uninstall", hwnd);
            Sleep(500);
            updateServiceStatus(state);
            break;
        case 3004: // Start service
            // Validar suscripción antes de permitir iniciar
            if (!validateServiceSubscription(state, hwnd)) {
                break;
            }
            saveServiceConfig(state);
            runServiceCommand(L"--start", hwnd);
            Sleep(500);
            updateServiceStatus(state);
            break;
        case 3005: // Stop service
            runServiceCommand(L"--stop", hwnd);
            Sleep(500);
            updateServiceStatus(state);
            break;
        case 3006: // Refresh status
            updateServiceStatus(state);
            break;
        case 3007: { // Generate code from server
            // Validar suscripción y contraseña de servicio
            if (!validateServiceSubscription(state, hwnd)) {
                break;
            }
            
            // Si llegamos aquí, la contraseña fue validada
            // Solicitar código disponible al servidor
            auto code = state->matchmaker->generateAvailableCode();
            if (code) {
                SetWindowTextA(state->serviceCodeEdit, code->c_str());
                vic::logging::global().log(vic::logging::Logger::Level::Info,
                    "Codigo generado del servidor: " + *code);
            } else {
                MessageBoxW(hwnd, L"No se pudo conectar al servidor para generar código.\nVerifica tu conexión a internet.", L"Error", MB_ICONWARNING);
            }
            break;
        }
        case IDM_TRAY_OPEN: // Abrir desde tray
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            break;
        case IDM_TRAY_CLOSE: // Cerrar desde tray
            DestroyWindow(hwnd);
            break;
        }
        break;
    }
    case WM_TIMER: {
        if (!state) break;
        
        // Timer de timeout de conexión del Viewer
        if (wParam == TIMER_VIEWER_CONNECT_TIMEOUT) {
            KillTimer(hwnd, TIMER_VIEWER_CONNECT_TIMEOUT);
            if (!state->viewerConnected) {
                PostMessage(hwnd, WM_VIEWER_TIMEOUT, 0, 0);
            }
        }
        
        // Timer de sesión FREE del Viewer (actualizar título cada segundo)
        if (wParam == TIMER_VIEWER_FREE_SESSION && state->viewerFreeMode && state->viewerConnected) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - state->viewerFreeStart).count();
            auto remaining = VIEWER_FREE_SESSION_MS - elapsed;
            
            if (remaining <= 0) {
                // Tiempo agotado - desconectar
                KillTimer(hwnd, TIMER_VIEWER_FREE_SESSION);
                state->viewerFreeTimer = 0;
                
                // REGISTRAR SESIÓN FREE TERMINADA (anti-abuso)
                AntiAbuse::instance().recordFreeSessionEnd();
                auto usage = AntiAbuse::instance().getUsageData();
                vic::logging::global().log(vic::logging::Logger::Level::Info,
                    "[UI] Viewer FREE: sesion #" + std::to_string(usage.sessionCount) + 
                    " terminada. Proxima espera: " + std::to_string(usage.currentWaitMinutes) + " min");
                
                // Mensaje de despedida con info de espera
                std::wstringstream msg;
                msg << L"GRACIAS POR USAR VICVIEWER\n\n";
                msg << L"Esta fue una sesion de cortesia.\n\n";
                msg << L"Gracias por ayudarnos a mantener este proyecto\n";
                msg << L"a costos honestamente accesibles.\n\n";
                msg << L"ADQUIERE UNA SUSCRIPCION PARA\n";
                msg << L"USO COMERCIAL EN:\n\n";
                msg << L"www.vicviewer.com\n\n";
                msg << L"Proxima sesion disponible en: " << usage.currentWaitMinutes << L" minutos\n\n";
                msg << L"Vicviewer - Control Remoto MX";
                
                MessageBoxW(hwnd, msg.str().c_str(), L"Sesion Finalizada", MB_ICONINFORMATION);
                
                // Desconectar viewer
                state->viewerSession->disconnect();
                state->viewerConnected = false;
                state->viewerFreeMode = false;
                // Restaurar ventana
                SetWindowLongPtr(hwnd, GWL_STYLE, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE);
                SetWindowPos(hwnd, HWND_TOP, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, SWP_NOMOVE | SWP_FRAMECHANGED);
                ShowWindow(state->viewerCodeEdit, SW_SHOW);
                ShowWindow(state->viewerButton, SW_SHOW);
                ShowWindow(state->viewerCanvas, SW_HIDE);
                SetWindowTextW(state->viewerButton, L"Conectar");
                SetWindowTextW(hwnd, L"Vicviewer");
            } else {
                // Actualizar titulo con tiempo restante
                int remainingSecs = static_cast<int>(remaining / 1000);
                int mins = remainingSecs / 60;
                int secs = remainingSecs % 60;
                wchar_t title[128];
                swprintf_s(title, L"Vicviewer - SESION DE CORTESIA FINALIZA EN: %d:%02d", mins, secs);
                SetWindowTextW(hwnd, title);
            }
        }
        
        // Timer ID 1: Métricas de streaming
        if (wParam == 1 && state->hostRunning) {
            // Actualizar métricas de streaming
            uint32_t fps = state->hostSession->currentFps();
            uint32_t bitrate = state->hostSession->currentBitrate();
            if (fps > 0 || bitrate > 0) {
                wchar_t metricsText[64];
                swprintf_s(metricsText, L"FPS: %u | Bitrate: %u kbps", fps, bitrate);
                SetWindowTextW(state->hostMetricsLabel, metricsText);
            }
            
            bool viewerConnectedNow = state->hostSession->isViewerConnected();
            
            // Si viewer conectó, minimizar a tray (solo una vez)
            if (viewerConnectedNow && !state->hiddenToTrayOnce) {
                state->hostHadViewerConnected = true;
                hideToTray(hwnd, state);
            }
            
            // Detectar desconexión del viewer - detener sesión, mostrar mensaje y restaurar ventana
            if (!viewerConnectedNow && state->hostHadViewerConnected) {
                state->hostHadViewerConnected = false;
                vic::logging::global().log(vic::logging::Logger::Level::Info,
                    "[UI] Host: Viewer se desconecto, deteniendo sesion y mostrando mensaje");
                
                // Primero detener la sesión
                stopHost(state);
                
                // Restaurar ventana del tray
                ShowWindow(hwnd, SW_SHOW);
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
                
                // Quitar icono del tray
                removeTrayIcon(state);
                
                // Mostrar mensaje
                MessageBoxW(hwnd, 
                    L"Esta fue una sesion de cortesia.\n\n"
                    L"Visite: www.vicviewer.com\n\n"
                    L"Vicviewer - Control Remoto MX",
                    L"Sesion Finalizada", MB_ICONINFORMATION);
            }
        }
        
        // Timer ID 2: Heartbeat (cada 60 segundos)
        if (wParam == 2 && !state->activeCode.empty()) {
            if (state->matchmaker->sendHeartbeat(state->activeCode)) {
                vic::logging::global().log(vic::logging::Logger::Level::Debug, 
                    "Heartbeat enviado: " + state->activeCode);
            }
        }
        
        // Timer ID 3: Sesión FREE - verificar tiempo restante
        if (wParam == 3 && state->isFreeMode && state->hostRunning) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - state->freeSessionStart).count();
            auto remaining = state->freeSessionMaxMs - elapsed;
            
            if (remaining <= 0) {
                // Tiempo agotado - detener sesión
                vic::logging::global().log(vic::logging::Logger::Level::Info,
                    "[UI] Sesion FREE: tiempo agotado, desconectando");
                MessageBoxW(hwnd, 
                    L"Su sesión gratuita de 5 minutos ha terminado.\n\n"
                    L"Debe esperar 10 minutos para iniciar una nueva sesión,\n"
                    L"o considere adquirir una suscripción para sesiones ilimitadas.",
                    L"Sesión Terminada", MB_ICONINFORMATION);
                stopHost(state);
            } else {
                // Actualizar contador en status
                int remainingSecs = static_cast<int>(remaining / 1000);
                int mins = remainingSecs / 60;
                int secs = remainingSecs % 60;
                wchar_t statusText[64];
                swprintf_s(statusText, L"GRATUITO - %d:%02d restantes", mins, secs);
                SetWindowTextW(state->hostStatus, statusText);
            }
        }
        break;
    }
    case WM_VIEWER_TIMEOUT: {
        if (!state) break;
        vic::logging::global().log(vic::logging::Logger::Level::Warning, "[UI] Timeout de conexion - no se recibio video");
        // Desconectar y restaurar UI
        state->viewerSession->disconnect();
        state->viewerConnected = false;
        SetWindowTextW(state->viewerButton, L"Conectar");
        EnableWindow(state->viewerButton, TRUE);
        MessageBoxW(state->mainWindow, L"No se pudo establecer la conexion.\nVerifica que el Host este activo y el codigo sea correcto.", L"Timeout de Conexion", MB_OK | MB_ICONWARNING);
        break;
    }
    case WM_VIEWER_CONNECTED: {
        if (!state) break;
        // Cambiar estilo de ventana a redimensionable
        SetWindowLongPtr(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        
        // Ocultar controles de conexion
        ShowWindow(state->viewerCodeEdit, SW_HIDE);
        ShowWindow(state->viewerButton, SW_HIDE);
        
        // Redimensionar ventana a 1280x720 (16:9) centrada
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        int winW = 1280, winH = 720;
        int posX = (screenW - winW) / 2;
        int posY = (screenH - winH) / 2;
        SetWindowPos(hwnd, HWND_TOP, posX, posY, winW, winH, SWP_SHOWWINDOW);
        
        // Ajustar canvas al area cliente
        RECT rect;
        GetClientRect(hwnd, &rect);
        MoveWindow(state->viewerCanvas, 0, 0, rect.right, rect.bottom, TRUE);
        ShowWindow(state->viewerCanvas, SW_SHOW);
        
        // Dar foco al canvas para que reciba eventos de teclado
        SetFocus(state->viewerCanvas);
        
        // MODO FREE: Sin companyCode = gratis con limite de 5 min
        // Con companyCode = depende de suscripcion (TODO: validar con servidor)
        if (state->companyCode.empty()) {
            state->viewerFreeMode = true;
            state->viewerFreeStart = std::chrono::steady_clock::now();
            state->viewerFreeTimer = SetTimer(hwnd, TIMER_VIEWER_FREE_SESSION, 1000, nullptr);
            SetWindowTextW(hwnd, L"Vicviewer - SESION DE CORTESIA FINALIZA EN: 5:00");
            vic::logging::global().log(vic::logging::Logger::Level::Info,
                "[UI] Viewer FREE mode - 5 minutos max (sin companyCode)");
        } else {
            state->viewerFreeMode = false;
            SetWindowTextW(hwnd, L"Vicviewer - Conectado");
        }
        break;
    }
    case WM_SIZE: {
        if (!state || wParam == SIZE_MINIMIZED) break;
        if (state->viewerConnected && state->viewerCanvas) {
            RECT rect;
            GetClientRect(hwnd, &rect);
            MoveWindow(state->viewerCanvas, 0, 0, rect.right, rect.bottom, TRUE);
        }
        break;
    }
    case WM_TRAYICON: {
        if (lParam == WM_LBUTTONUP) {
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        } else if (lParam == WM_RBUTTONUP) {
            // Mostrar menú contextual
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, IDM_TRAY_OPEN, L"Abrir Vicviewer");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, IDM_TRAY_CLOSE, L"Cerrar");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
        }
        break;
    }
    case WM_CLOSE: {
        if (!state) {
            DestroyWindow(hwnd);
            break;
        }
        
        // Si el servicio está instalado y corriendo, minimizar a tray
        if (isServiceInstalled() && state->hostRunning) {
            hideToTray(hwnd, state);
            return 0;  // No cerrar
        }
        
        // Si no hay servicio, cerrar todo
        DestroyWindow(hwnd);
        break;
    }
    case WM_DESTROY: {
        if (state) {
            // Enviar desconexión limpia al servidor
            if (!state->activeCode.empty()) {
                state->matchmaker->disconnect(state->activeCode);
                KillTimer(hwnd, 2);  // Detener heartbeat
            }
            state->hostSession->stop();
            state->viewerSession->disconnect();
            removeTrayIcon(state);
            
            // Liberar bitmap del banner
            if (state->bannerBitmap) {
                DeleteObject(state->bannerBitmap);
                state->bannerBitmap = nullptr;
            }
        }
        cleanupGdiResources();
        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK ViewerCanvasProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto state = reinterpret_cast<MainWindowState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!state) {
        return g_originalCanvasProc ? CallWindowProc(g_originalCanvasProc, hwnd, msg, wParam, lParam)
                                    : DefWindowProc(hwnd, msg, wParam, lParam);
    }
    
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);
        
        std::lock_guard lock(state->frameMutex);
        if (state->lastFrame) {
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = static_cast<LONG>(state->lastFrame->width);
            bmi.bmiHeader.biHeight = -static_cast<LONG>(state->lastFrame->height);
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            StretchDIBits(hdc, 0, 0, rect.right, rect.bottom,
                0, 0, state->lastFrame->width, state->lastFrame->height,
                state->lastFrame->bgraData.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            FillRect(hdc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEWHEEL: {
        if (msg == WM_LBUTTONDOWN) SetFocus(hwnd);
        if (!state->viewerSession->isConnected()) {
            if (msg == WM_LBUTTONDOWN) {
                vic::logging::global().log(vic::logging::Logger::Level::Warning, "[UI] Click ignorado: no conectado");
            }
            break;
        }
        
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT rect;
        GetClientRect(hwnd, &rect);
        
        std::lock_guard lock(state->frameMutex);
        if (!state->lastFrame) {
            if (msg == WM_LBUTTONDOWN) {
                vic::logging::global().log(vic::logging::Logger::Level::Warning, "[UI] Click ignorado: sin frame");
            }
            break;
        }

        float fx = static_cast<float>(pt.x) / rect.right;
        float fy = static_cast<float>(pt.y) / rect.bottom;

        // Usar originalWidth/Height para coordenadas correctas (compensar escalado)
        uint32_t targetW = state->lastFrame->originalWidth > 0 ? state->lastFrame->originalWidth : state->lastFrame->width;
        uint32_t targetH = state->lastFrame->originalHeight > 0 ? state->lastFrame->originalHeight : state->lastFrame->height;

        vic::input::MouseEvent ev{};
        ev.absolute = true;  // Coordenadas absolutas
        ev.x = static_cast<int32_t>(fx * targetW);
        ev.y = static_cast<int32_t>(fy * targetH);

        switch (msg) {
        case WM_MOUSEMOVE: ev.action = vic::input::MouseAction::Move; break;
        case WM_LBUTTONDOWN: ev.action = vic::input::MouseAction::Down; ev.button = vic::input::MouseButton::Left; break;
        case WM_LBUTTONUP: ev.action = vic::input::MouseAction::Up; ev.button = vic::input::MouseButton::Left; break;
        case WM_RBUTTONDOWN: ev.action = vic::input::MouseAction::Down; ev.button = vic::input::MouseButton::Right; break;
        case WM_RBUTTONUP: ev.action = vic::input::MouseAction::Up; ev.button = vic::input::MouseButton::Right; break;
        case WM_MOUSEWHEEL: ev.action = vic::input::MouseAction::Wheel; ev.wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam); break;
        }

        bool sent = state->viewerSession->sendMouseEvent(ev);
        if (msg == WM_LBUTTONDOWN) {
            vic::logging::global().log(vic::logging::Logger::Level::Info, 
                "[UI] Mouse click enviado: x=" + std::to_string(ev.x) + " y=" + std::to_string(ev.y) + 
                " sent=" + std::string(sent ? "true" : "false"));
        }
        break;
    }
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        if (!state->viewerSession->isConnected()) break;
        vic::input::KeyboardEvent kev{};
        kev.virtualKey = static_cast<uint16_t>(wParam);
        kev.scanCode = static_cast<uint16_t>((lParam >> 16) & 0xFF);
        kev.action = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) ? vic::input::KeyAction::Down : vic::input::KeyAction::Up;
        kev.extended = ((lParam >> 24) & 1) != 0;  // Extended key flag
        state->viewerSession->sendKeyboardEvent(kev);
        
        // Loguear para debug
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
            vic::logging::global().log(vic::logging::Logger::Level::Debug,
                "[UI] Key enviada: vk=" + std::to_string(kev.virtualKey) + 
                " scan=" + std::to_string(kev.scanCode));
        }
        return 0;  // No pasar al handler por defecto
    }
    default:
        return g_originalCanvasProc ? CallWindowProc(g_originalCanvasProc, hwnd, msg, wParam, lParam)
                                    : DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

} // namespace

int run(HINSTANCE instance, int showCommand, vic::AppContext& /*context*/, const LaunchOptions& options) {
    INITCOMMONCONTROLSEX icex{sizeof(icex), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icex);

    const wchar_t* className = L"VicViewerMainWindow";

    WNDCLASSEX wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = getAppIcon(instance);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = className;

    if (!RegisterClassExW(&wc)) return -1;

    auto state = std::make_unique<MainWindowState>();
    
    // Extraer clientId del nombre del ejecutable (ej: VicViewer1234.exe -> "1234")
    state->clientId = extractClientIdFromExeName();
    
    // Leer configuracion de NO_AUTO_CODE antes de decidir auto-start
    bool noAutoCode = readNoAutoCodeSetting();
    
    // Si hay companyCode, configurarlo (la validacion se hace al conectar como viewer)
    if (!state->clientId.empty()) {
        state->companyCode = state->clientId;  // Sincronizar companyCode
        // Solo auto-iniciar si NO esta marcada la opcion "No generar codigo al inicio"
        if (!noAutoCode) {
            state->autoStartPending = true;
            vic::logging::global().log(vic::logging::Logger::Level::Info,
                "[UI] ClientID detectado: " + state->clientId + " - Se auto-iniciara compartir");
        } else {
            vic::logging::global().log(vic::logging::Logger::Level::Info,
                "[UI] ClientID detectado: " + state->clientId + " - Auto-inicio desactivado por config");
        }
    }
    
    // Aplicar código fijo desde opciones de línea de comandos
    if (!options.sessionCode.empty()) {
        state->fixedCode = wideToUtf8(options.sessionCode);
        vic::logging::global().log(vic::logging::Logger::Level::Info,
            "[UI] Codigo fijo configurado desde linea de comandos: " + state->fixedCode);
    }

    // Calculate window size for client area
    RECT wr = {0, 0, WINDOW_WIDTH, WINDOW_HEIGHT};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, FALSE);
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;

    HWND hwnd = CreateWindowExW(0, className, L"Vicviewer",
        (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME),
        CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
        nullptr, nullptr, instance, state.get());

    if (!hwnd) return -1;

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    // Auto-iniciar compartir si hay clientId configurado
    if (state->autoStartPending) {
        state->autoStartPending = false;
        vic::logging::global().log(vic::logging::Logger::Level::Info,
            "[UI] Auto-iniciando compartir pantalla (clientId: " + state->clientId + ")");
        // Usar PostMessage para iniciar después de que la UI esté lista
        PostMessage(hwnd, WM_COMMAND, MAKEWPARAM(1002, BN_CLICKED), 0);  // 1002 = botón Compartir
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return static_cast<int>(msg.wParam);
}

} // namespace vic::ui
