#include "Logger.h"

#include <iomanip>
#include <iostream>
#include <sstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

using namespace std::chrono;

namespace vic::logging {

namespace {
    Logger g_logger{};
    
    std::wstring getExeDirectory() {
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
}

Logger::Logger() {
    // Siempre escribir a local.log en el directorio del ejecutable
    std::wstring logPath = getExeDirectory() + L"local.log";
    try {
        setFilePath(logPath);
    } catch (...) {
        // Ignorar fallo silenciosamente.
    }
    
    // Override con variable de entorno si existe
    if (const wchar_t* path = _wgetenv(L"VIC_LOG_FILE")) {
        try {
            setFilePath(path);
        } catch (...) {
            // Ignorar fallo silenciosamente.
        }
    }
    if (const wchar_t* disableConsole = _wgetenv(L"VIC_LOG_NO_CONSOLE")) {
        toConsole_ = false;
    }
}

Logger::Logger(const std::wstring& filePath) {
    setFilePath(filePath);
}

void Logger::setFilePath(const std::wstring& filePath) {
    std::lock_guard lock(mutex_);
    file_.open(filePath, std::ios::out | std::ios::app);
    file_.imbue(std::locale(""));
}

void Logger::log(Level level, const std::string& message) {
    write(level, message);
}

void Logger::write(Level level, const std::string& message) {
    auto now = system_clock::now();
    auto now_time = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << ms.count()
        << " [";

    switch (level) {
    case Level::Debug:
        oss << "DEBUG";
        break;
    case Level::Info:
        oss << "INFO";
        break;
    case Level::Warning:
        oss << "WARN";
        break;
    case Level::Error:
        oss << "ERROR";
        break;
    }

    oss << "] " << message << '\n';

    auto line = oss.str();

    std::lock_guard lock(mutex_);
    if (toConsole_) {
        std::cout << line;
        std::cout.flush();
    }
    if (file_.is_open()) {
        // Escribir directamente sin conversión problemática
        for (char c : line) {
            file_.put(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
        file_.flush();
    }
}

Logger& global() {
    return g_logger;
}

} // namespace vic::logging
