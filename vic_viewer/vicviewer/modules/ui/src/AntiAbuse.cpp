#include "vic/ui/AntiAbuse.h"
#include "Logger.h"

#include <Windows.h>
#include <ShlObj.h>
#include <iphlpapi.h>
#include <intrin.h>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <chrono>
#include <algorithm>

#pragma comment(lib, "iphlpapi.lib")

namespace vic {
namespace ui {

namespace {
    // Claves ofuscadas para el registro (no obvias)
    const wchar_t* REG_KEY_PATH = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced\\Folder\\Hidden";
    const wchar_t* REG_VALUE_NAME = L"SysCache";
    
    // Nombres de archivo ofuscados
    const char* APPDATA_FILENAME = ".wincache.dat";
    const char* PROGRAMDATA_FILENAME = ".syscache.dat";
    
    // XOR key para ofuscación simple
    const char XOR_KEY[] = "V1cV13w3r$3cur1ty";

    int64_t getCurrentTimestamp() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

AntiAbuse& AntiAbuse::instance() {
    static AntiAbuse instance;
    return instance;
}

AntiAbuse::AntiAbuse() {
    machineId_ = generateMachineId();
    cachedData_ = loadData();
    dataLoaded_ = true;
    
    vic::logging::global().log(vic::logging::Logger::Level::Debug,
        "[AntiAbuse] Inicializado - Sesiones: " + std::to_string(cachedData_.sessionCount) +
        ", Espera: " + std::to_string(cachedData_.currentWaitMinutes) + " min");
}

std::string AntiAbuse::generateMachineId() {
    std::stringstream ss;
    
    // 1. CPUID
    int cpuInfo[4] = {0};
    __cpuid(cpuInfo, 0);
    ss << std::hex << cpuInfo[0] << cpuInfo[1] << cpuInfo[2] << cpuInfo[3];
    
    // 2. Nombre del computador
    char computerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(computerName);
    if (GetComputerNameA(computerName, &size)) {
        ss << computerName;
    }
    
    // 3. Volume serial del disco C:
    DWORD volumeSerial = 0;
    if (GetVolumeInformationA("C:\\", NULL, 0, &volumeSerial, NULL, NULL, NULL, 0)) {
        ss << std::hex << volumeSerial;
    }
    
    // 4. MAC address de la primera interfaz
    IP_ADAPTER_INFO adapterInfo[16];
    DWORD bufLen = sizeof(adapterInfo);
    if (GetAdaptersInfo(adapterInfo, &bufLen) == ERROR_SUCCESS) {
        for (int i = 0; i < 6; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') 
               << (int)adapterInfo[0].Address[i];
        }
    }
    
    // Hash simple del string resultante
    std::string combined = ss.str();
    uint32_t hash = 0;
    for (char c : combined) {
        hash = hash * 31 + c;
    }
    
    std::stringstream result;
    result << std::hex << std::setw(8) << std::setfill('0') << hash;
    return result.str();
}

uint32_t AntiAbuse::calculateWaitMinutes(uint32_t sessionCount) {
    // Escala progresiva:
    // 0-49 sesiones: 10 minutos
    // 50-99 sesiones: 20 minutos
    // 100-199 sesiones: 2 horas (120 min)
    // 200+ sesiones: 4 horas (240 min)
    
    if (sessionCount < 50) {
        return 10;
    } else if (sessionCount < 100) {
        return 20;
    } else if (sessionCount < 200) {
        return 120;  // 2 horas
    } else {
        return 240;  // 4 horas
    }
}

bool AntiAbuse::canStartFreeSession() {
    return getWaitSecondsRemaining() <= 0;
}

int64_t AntiAbuse::getWaitSecondsRemaining() {
    if (cachedData_.lastSessionEnd == 0) {
        return 0;  // Primera vez, puede conectar
    }
    
    int64_t now = getCurrentTimestamp();
    int64_t waitSeconds = cachedData_.currentWaitMinutes * 60;
    int64_t elapsed = now - cachedData_.lastSessionEnd;
    int64_t remaining = waitSeconds - elapsed;
    
    return (remaining > 0) ? remaining : 0;
}

void AntiAbuse::recordFreeSessionEnd() {
    cachedData_.sessionCount++;
    cachedData_.lastSessionEnd = getCurrentTimestamp();
    cachedData_.currentWaitMinutes = calculateWaitMinutes(cachedData_.sessionCount);
    
    saveData(cachedData_);
    
    vic::logging::global().log(vic::logging::Logger::Level::Info,
        "[AntiAbuse] Sesion FREE #" + std::to_string(cachedData_.sessionCount) +
        " registrada. Proxima espera: " + std::to_string(cachedData_.currentWaitMinutes) + " min");
}

AntiAbuse::UsageData AntiAbuse::getUsageData() {
    return cachedData_;
}

std::wstring AntiAbuse::getWaitMessage() {
    int64_t remaining = getWaitSecondsRemaining();
    int mins = static_cast<int>(remaining / 60);
    int secs = static_cast<int>(remaining % 60);
    
    std::wstringstream ss;
    
    // Si es usuario frecuente (mas de 50 sesiones), mensaje diferente
    if (cachedData_.sessionCount >= 50) {
        ss << L"GRACIAS POR USAR VICVIEWER\n\n";
        ss << L"Debido al uso frecuente de este servicio,\n";
        ss << L"debe esperar un poco mas entre sesiones.\n\n";
    } else {
        ss << L"SESION DE CORTESIA FINALIZADA\n\n";
    }
    
    ss << L"Tiempo de espera: " << mins << L":" 
       << std::setw(2) << std::setfill(L'0') << secs << L"\n\n";
    
    ss << L"Gracias por ayudarnos a mantener este proyecto\n";
    ss << L"a costos honestamente accesibles.\n\n";
    ss << L"ADQUIERE UNA SUSCRIPCION PARA\n";
    ss << L"USO COMERCIAL EN:\n\n";
    ss << L"www.vicviewer.com\n\n";
    ss << L"Vicviewer - Control Remoto MX";
    
    return ss.str();
}

std::string AntiAbuse::encryptData(const std::string& data) {
    std::string result = data;
    size_t keyLen = strlen(XOR_KEY);
    for (size_t i = 0; i < result.size(); i++) {
        result[i] ^= XOR_KEY[i % keyLen];
    }
    
    // Convertir a hex para almacenamiento seguro
    std::stringstream ss;
    for (unsigned char c : result) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    }
    return ss.str();
}

std::string AntiAbuse::decryptData(const std::string& hexData) {
    // Convertir de hex
    std::string data;
    for (size_t i = 0; i < hexData.length(); i += 2) {
        std::string byteStr = hexData.substr(i, 2);
        char byte = (char)strtol(byteStr.c_str(), nullptr, 16);
        data += byte;
    }
    
    // XOR decrypt
    size_t keyLen = strlen(XOR_KEY);
    for (size_t i = 0; i < data.size(); i++) {
        data[i] ^= XOR_KEY[i % keyLen];
    }
    return data;
}

void AntiAbuse::saveData(const UsageData& data) {
    saveToRegistry(data);
    saveToAppData(data);
    saveToProgramData(data);
}

void AntiAbuse::saveToRegistry(const UsageData& data) {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, NULL, 
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        
        // Formato: machineId|sessionCount|lastSessionEnd|waitMinutes
        std::stringstream ss;
        ss << machineId_ << "|" << data.sessionCount << "|" 
           << data.lastSessionEnd << "|" << data.currentWaitMinutes;
        
        std::string encrypted = encryptData(ss.str());
        
        RegSetValueExW(hKey, REG_VALUE_NAME, 0, REG_SZ, 
                       (const BYTE*)encrypted.c_str(), 
                       (DWORD)(encrypted.size() + 1));
        RegCloseKey(hKey);
    }
}

void AntiAbuse::saveToAppData(const UsageData& data) {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path))) {
        std::string filePath = std::string(path) + "\\" + APPDATA_FILENAME;
        
        std::stringstream ss;
        ss << machineId_ << "|" << data.sessionCount << "|" 
           << data.lastSessionEnd << "|" << data.currentWaitMinutes;
        
        std::string encrypted = encryptData(ss.str());
        
        std::ofstream file(filePath, std::ios::binary);
        if (file.is_open()) {
            file << encrypted;
            file.close();
            
            // Ocultar archivo
            SetFileAttributesA(filePath.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        }
    }
}

void AntiAbuse::saveToProgramData(const UsageData& data) {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, path))) {
        std::string filePath = std::string(path) + "\\" + PROGRAMDATA_FILENAME;
        
        std::stringstream ss;
        ss << machineId_ << "|" << data.sessionCount << "|" 
           << data.lastSessionEnd << "|" << data.currentWaitMinutes;
        
        std::string encrypted = encryptData(ss.str());
        
        std::ofstream file(filePath, std::ios::binary);
        if (file.is_open()) {
            file << encrypted;
            file.close();
            
            // Ocultar archivo
            SetFileAttributesA(filePath.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
        }
    }
}

AntiAbuse::UsageData AntiAbuse::loadData() {
    // Intentar cargar de todas las fuentes, usar la que tenga más sesiones
    UsageData reg = loadFromRegistry();
    UsageData appData = loadFromAppData();
    UsageData progData = loadFromProgramData();
    
    // Usar los datos con mayor contador de sesiones (más difícil de manipular)
    UsageData best = reg;
    if (appData.sessionCount > best.sessionCount) best = appData;
    if (progData.sessionCount > best.sessionCount) best = progData;
    
    // Si encontramos datos, sincronizar todas las ubicaciones
    if (best.sessionCount > 0) {
        saveData(best);
    }
    
    return best;
}

AntiAbuse::UsageData AntiAbuse::loadFromRegistry() {
    UsageData data;
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char buffer[256] = {0};
        DWORD bufSize = sizeof(buffer);
        DWORD type;
        
        if (RegQueryValueExW(hKey, REG_VALUE_NAME, NULL, &type, 
                             (LPBYTE)buffer, &bufSize) == ERROR_SUCCESS) {
            try {
                std::string decrypted = decryptData(std::string(buffer));
                
                // Parse: machineId|sessionCount|lastSessionEnd|waitMinutes
                std::stringstream ss(decrypted);
                std::string token;
                std::vector<std::string> parts;
                while (std::getline(ss, token, '|')) {
                    parts.push_back(token);
                }
                
                if (parts.size() >= 4 && parts[0] == machineId_) {
                    data.sessionCount = std::stoul(parts[1]);
                    data.lastSessionEnd = std::stoll(parts[2]);
                    data.currentWaitMinutes = std::stoul(parts[3]);
                }
            } catch (...) {
                // Datos corruptos, ignorar
            }
        }
        RegCloseKey(hKey);
    }
    return data;
}

AntiAbuse::UsageData AntiAbuse::loadFromAppData() {
    UsageData data;
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path))) {
        std::string filePath = std::string(path) + "\\" + APPDATA_FILENAME;
        
        std::ifstream file(filePath, std::ios::binary);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            file.close();
            
            try {
                std::string decrypted = decryptData(buffer.str());
                
                std::stringstream ss(decrypted);
                std::string token;
                std::vector<std::string> parts;
                while (std::getline(ss, token, '|')) {
                    parts.push_back(token);
                }
                
                if (parts.size() >= 4 && parts[0] == machineId_) {
                    data.sessionCount = std::stoul(parts[1]);
                    data.lastSessionEnd = std::stoll(parts[2]);
                    data.currentWaitMinutes = std::stoul(parts[3]);
                }
            } catch (...) {
                // Datos corruptos, ignorar
            }
        }
    }
    return data;
}

AntiAbuse::UsageData AntiAbuse::loadFromProgramData() {
    UsageData data;
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, path))) {
        std::string filePath = std::string(path) + "\\" + PROGRAMDATA_FILENAME;
        
        std::ifstream file(filePath, std::ios::binary);
        if (file.is_open()) {
            std::stringstream buffer;
            buffer << file.rdbuf();
            file.close();
            
            try {
                std::string decrypted = decryptData(buffer.str());
                
                std::stringstream ss(decrypted);
                std::string token;
                std::vector<std::string> parts;
                while (std::getline(ss, token, '|')) {
                    parts.push_back(token);
                }
                
                if (parts.size() >= 4 && parts[0] == machineId_) {
                    data.sessionCount = std::stoul(parts[1]);
                    data.lastSessionEnd = std::stoll(parts[2]);
                    data.currentWaitMinutes = std::stoul(parts[3]);
                }
            } catch (...) {
                // Datos corruptos, ignorar
            }
        }
    }
    return data;
}

} // namespace ui
} // namespace vic
