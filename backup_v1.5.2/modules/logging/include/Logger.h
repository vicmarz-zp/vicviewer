#pragma once

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>

namespace vic::logging {

class Logger {
public:
    enum class Level {
        Debug,
        Info,
        Warning,
        Error
    };

    Logger();
    explicit Logger(const std::wstring& filePath);

    void log(Level level, const std::string& message);
    void setFilePath(const std::wstring& filePath);

private:
    void write(Level level, const std::string& message);

    std::wofstream file_;
    std::mutex mutex_;
    bool toConsole_ = true;
};

Logger& global();

} // namespace vic::logging
