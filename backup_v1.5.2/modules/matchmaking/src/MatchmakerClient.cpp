#include "MatchmakerClient.h"

#include "Logger.h"

#include <Windows.h>
#include <winhttp.h>

#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <utility>

#pragma comment(lib, "Winhttp.lib")

namespace vic::matchmaking {

namespace {
    void appendUtf8Codepoint(std::string& output, unsigned int codepoint);

struct ParsedUrl {
    std::wstring host;
    INTERNET_PORT port{};
    std::wstring path;
    bool secure = false;
};

std::optional<ParsedUrl> parseUrl(const std::wstring& url) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(URL_COMPONENTS);
    components.dwHostNameLength = -1;
    components.dwUrlPathLength = -1;
    components.dwSchemeLength = -1;

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        return std::nullopt;
    }

    ParsedUrl result;
    result.host.assign(components.lpszHostName, components.dwHostNameLength);
    result.port = components.nPort;
    result.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    result.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return result;
}

std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::optional<std::string> readResponse(HINTERNET request) {
    std::string response;
    DWORD available = 0;
    do {
        if (!WinHttpQueryDataAvailable(request, &available)) {
            break;
        }
        if (!available) {
            break;
        }
        std::string buffer(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read)) {
            break;
        }
        buffer.resize(read);
        response.append(buffer);
    } while (available > 0);
    if (response.empty()) {
        return std::nullopt;
    }
    return response;
}

std::string jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 16);
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                escaped += buffer;
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return escaped;
}

std::string jsonUnescape(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        char ch = value[i];
        if (ch == '\\' && i + 1 < value.size()) {
            char next = value[++i];
            switch (next) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                if (i + 4 < value.size()) {
                    unsigned int codepoint = 0;
                    bool valid = true;
                    for (size_t j = 0; j < 4; ++j) {
                        char hex = value[i + 1 + j];
                        codepoint <<= 4;
                        if (hex >= '0' && hex <= '9') {
                            codepoint |= static_cast<unsigned int>(hex - '0');
                        } else if (hex >= 'a' && hex <= 'f') {
                            codepoint |= static_cast<unsigned int>(10 + hex - 'a');
                        } else if (hex >= 'A' && hex <= 'F') {
                            codepoint |= static_cast<unsigned int>(10 + hex - 'A');
                        } else {
                            valid = false;
                            break;
                        }
                    }
                    i += 4;
                    if (valid) {
                        appendUtf8Codepoint(output, codepoint);
                    }
                }
                break;
            }
            default:
                output.push_back(next);
                break;
            }
        } else {
            output.push_back(ch);
        }
    }
    return output;
}

std::optional<std::string> extractJsonField(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find('"', pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    auto end = json.find('"', pos + 1);
    while (end != std::string::npos && json[end - 1] == '\\') {
        end = json.find('"', end + 1);
    }
    if (end == std::string::npos) {
        return std::nullopt;
    }
    return jsonUnescape(json.substr(pos + 1, end - (pos + 1)));
}

std::optional<int> extractJsonInt(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    auto end = json.find_first_of(",}\n", pos + 1);
    if (end == std::string::npos) {
        end = json.size();
    }
    std::string number = json.substr(pos + 1, end - (pos + 1));
    try {
        return std::stoi(number);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> extractJsonBool(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    // Skip whitespace after ':'
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
        pos++;
    }
    // Check for true/false boolean values
    if (json.substr(pos, 4) == "true") {
        return true;
    }
    if (json.substr(pos, 5) == "false") {
        return false;
    }
    // Also check for "true"/"false" as strings
    if (json.substr(pos, 6) == "\"true\"") {
        return true;
    }
    if (json.substr(pos, 7) == "\"false\"") {
        return false;
    }
    return std::nullopt;
}

std::optional<std::string> extractJsonObject(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find('{', pos);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    int depth = 0;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '{') {
            ++depth;
        } else if (json[i] == '}') {
            --depth;
            if (depth == 0) {
                return json.substr(pos, i - pos + 1);
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string> extractJsonObjectArray(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string pattern = "\"" + key + "\"";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) {
        return result;
    }
    pos = json.find('[', pos);
    if (pos == std::string::npos) {
        return result;
    }
    ++pos;
    int depth = 0;
    size_t start = pos;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '{') {
            if (depth == 0) {
                start = i;
            }
            ++depth;
        } else if (json[i] == '}') {
            --depth;
            if (depth == 0) {
                result.push_back(json.substr(start, i - start + 1));
            }
        } else if (json[i] == ']' && depth == 0) {
            break;
        }
    }
    return result;
}

void appendUtf8Codepoint(std::string& output, unsigned int codepoint) {
    if (codepoint <= 0x7F) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

} // namespace

MatchmakerClient::MatchmakerClient(std::wstring serviceUrl)
    : serviceUrl_(std::move(serviceUrl)) {
}

const std::wstring& MatchmakerClient::effectiveUrl() const {
    return usingFallback_ ? fallbackUrl_ : serviceUrl_;
}

const std::wstring& MatchmakerClient::currentUrl() const {
    return effectiveUrl();
}

bool MatchmakerClient::tryFallback() {
    if (usingFallback_) {
        return false;  // Ya estamos en fallback, no hay más opciones
    }
    logging::global().log(logging::Logger::Level::Info, 
        "Primary URL failed, switching to fallback: " + wideToUtf8(fallbackUrl_));
    usingFallback_ = true;
    return true;
}

std::optional<std::string> MatchmakerClient::registerHost(const vic::transport::ConnectionInfo& info) {
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) {
        // Si falla el parseo de la URL principal, intentar fallback
        if (tryFallback()) {
            parsed = parseUrl(effectiveUrl());
        }
        if (!parsed) {
            logging::global().log(logging::Logger::Level::Error, "Failed to parse matchmaker URL");
            return std::nullopt;
        }
    }

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) {
        return std::nullopt;
    }
    // Establecer timeouts razonables para evitar bloqueos prolongados
    WinHttpSetTimeouts(session, 3000, 5000, 5000, 5000);

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        // Intentar fallback si la conexión falla (ej: DNS no resuelve)
        if (tryFallback()) {
            logging::global().log(logging::Logger::Level::Info, 
                "registerHost: Connection failed, retrying with fallback");
            return registerHost(info);
        }
        return std::nullopt;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') {
        path.pop_back();
    }
    path += L"/register";

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::ostringstream body;
    body << '{';
    body << "\"code\":\"" << jsonEscape(info.code) << "\",";
    // Incluir companyCode (nuevo) y clientId (legacy) si están configurados
    if (!companyCode_.empty()) {
        body << "\"companyCode\":\"" << jsonEscape(companyCode_) << "\",";
    }
    if (!clientId_.empty()) {
        body << "\"clientId\":\"" << jsonEscape(clientId_) << "\",";
    }
    // Incluir serial de disco para modo free
    if (!diskSerial_.empty()) {
        body << "\"diskSerial\":\"" << jsonEscape(diskSerial_) << "\",";
    }
    // Incluir isService para modo servicio
    if (isService_) {
        body << "\"isService\":true,";
        if (!deviceName_.empty()) {
            body << "\"deviceName\":\"" << jsonEscape(deviceName_) << "\",";
        }
    }
    body << "\"offer\":{\"type\":\"" << jsonEscape(info.offer.type) << "\",\"sdp\":\"" << jsonEscape(info.offer.sdp) << "\"},";
    body << "\"iceCandidates\":[";
    for (size_t i = 0; i < info.iceCandidates.size(); ++i) {
        const auto& candidate = info.iceCandidates[i];
        body << "{\"candidate\":\"" << jsonEscape(candidate.candidate) << "\",\"sdpMid\":\"" << jsonEscape(candidate.sdpMid) << "\",\"sdpMLineIndex\":" << candidate.sdpMLineIndex << '}';
        if (i + 1 < info.iceCandidates.size()) {
            body << ',';
        }
    }
    body << "],";
    body << "\"iceServers\":[";
    for (size_t i = 0; i < info.iceServers.size(); ++i) {
        const auto& server = info.iceServers[i];
        body << "{\"url\":\"" << jsonEscape(server.url) << "\"";
        if (server.username) {
            body << ",\"username\":\"" << jsonEscape(*server.username) << "\"";
        }
        if (server.credential) {
            body << ",\"credential\":\"" << jsonEscape(*server.credential) << "\"";
        }
        if (server.relayTransport) {
            body << ",\"relay\":\"" << jsonEscape(*server.relayTransport) << "\"";
        }
        body << '}';
        if (i + 1 < info.iceServers.size()) {
            body << ',';
        }
    }
    body << "]}";
    logging::global().log(logging::Logger::Level::Debug, "Matchmaker register payload: " + body.str());

    std::string payload = body.str();

    BOOL result = WinHttpSendRequest(request,
        L"Content-Type: application/json\r\n",
        -1,
        payload.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(payload.data()),
        payload.empty() ? 0 : static_cast<DWORD>(payload.size()),
        payload.empty() ? 0 : static_cast<DWORD>(payload.size()),
        0);

    if (!result || !WinHttpReceiveResponse(request, nullptr)) {
        DWORD err = GetLastError();
        logging::global().log(logging::Logger::Level::Error, 
            "registerHost: HTTP request failed, error=" + std::to_string(err));
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        // Intentar fallback si la solicitud falla
        if (tryFallback()) {
            logging::global().log(logging::Logger::Level::Info, 
                "registerHost: Retrying with fallback URL");
            return registerHost(info);
        }
        return std::nullopt;
    }

    // Leer status code
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    auto response = readResponse(request);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    logging::global().log(logging::Logger::Level::Info, 
        "registerHost: HTTP " + std::to_string(statusCode) + " response: " + (response ? response->substr(0, 200) : "null"));

    if (!response) {
        return std::nullopt;
    }
    
    // Si hay error HTTP, retornar nullopt
    if (statusCode >= 400) {
        logging::global().log(logging::Logger::Level::Error, 
            "registerHost: Server returned error " + std::to_string(statusCode));
        return std::nullopt;
    }
    
    if (auto assignedCode = extractJsonField(*response, "code")) {
        return assignedCode;
    }
    return info.code;
}

std::optional<vic::transport::ConnectionInfo> MatchmakerClient::resolveCode(const std::string& code) {
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) {
        if (tryFallback()) {
            logging::global().log(logging::Logger::Level::Info, "resolveCode: URL parse failed, trying fallback");
            return resolveCode(code);
        }
        return std::nullopt;
    }

    logging::global().log(logging::Logger::Level::Info, 
        "resolveCode: Connecting to " + wideToUtf8(effectiveUrl()) + " for code " + code);

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) {
        logging::global().log(logging::Logger::Level::Error, "resolveCode: WinHttpOpen failed");
        return std::nullopt;
    }
    WinHttpSetTimeouts(session, 5000, 10000, 10000, 10000);

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        if (tryFallback()) {
            logging::global().log(logging::Logger::Level::Info, "resolveCode: Connection failed, trying fallback");
            return resolveCode(code);
        }
        return std::nullopt;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') {
        path.pop_back();
    }
    path += L"/resolve?code=";
    path += utf8ToWide(code);

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (tryFallback()) {
            logging::global().log(logging::Logger::Level::Info, "resolveCode: HTTP request failed, trying fallback");
            return resolveCode(code);
        }
        return std::nullopt;
    }

    auto response = readResponse(request);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!response) {
        logging::global().log(logging::Logger::Level::Warning, "resolveCode: Empty response for code " + code);
        return std::nullopt;
    }

    logging::global().log(logging::Logger::Level::Info,
        "resolveCode: Got response for code " + code);

    logging::global().log(logging::Logger::Level::Debug,
        "Matchmaker resolve raw response: " + *response);

    vic::transport::ConnectionInfo info{};
    info.code = code;

    if (auto offerObj = extractJsonObject(*response, "offer")) {
        if (auto type = extractJsonField(*offerObj, "type")) {
            info.offer.type = *type;
        }
        if (auto sdp = extractJsonField(*offerObj, "sdp")) {
            info.offer.sdp = *sdp;
        }
    }

    auto candidateEntries = extractJsonObjectArray(*response, "iceCandidates");
    for (const auto& entry : candidateEntries) {
        vic::transport::IceCandidate candidate{};
        candidate.candidate = extractJsonField(entry, "candidate").value_or("");
        candidate.sdpMid = extractJsonField(entry, "sdpMid").value_or("");
        if (auto idx = extractJsonInt(entry, "sdpMLineIndex")) {
            candidate.sdpMLineIndex = *idx;
        }
        info.iceCandidates.push_back(std::move(candidate));
    }

    auto serverEntries = extractJsonObjectArray(*response, "iceServers");
    for (const auto& entry : serverEntries) {
        vic::transport::IceServer server{};
        server.url = extractJsonField(entry, "url").value_or("");
        if (auto user = extractJsonField(entry, "username")) {
            server.username = *user;
        }
        if (auto pass = extractJsonField(entry, "credential")) {
            server.credential = *pass;
        }
        if (auto relay = extractJsonField(entry, "relay")) {
            server.relayTransport = *relay;
        }
        if (!server.url.empty()) {
            info.iceServers.push_back(std::move(server));
        }
    }

    if (info.offer.sdp.empty()) {
        logging::global().log(logging::Logger::Level::Warning,
            "Matchmaker resolve: oferta sin SDP para código " + code);
        return std::nullopt;
    }

    return info;
}

std::optional<vic::transport::AnswerBundle> MatchmakerClient::fetchViewerAnswer(const std::string& code) {
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) {
        return std::nullopt;
    }

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) {
        return std::nullopt;
    }

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') {
        path.pop_back();
    }
    path += L"/answer?code=" + utf8ToWide(code);

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    auto response = readResponse(request);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!response) {
        return std::nullopt;
    }

    vic::transport::AnswerBundle bundle{};
    if (auto answerObj = extractJsonObject(*response, "answer")) {
        if (auto type = extractJsonField(*answerObj, "type")) {
            bundle.description.type = *type;
        }
        if (auto sdp = extractJsonField(*answerObj, "sdp")) {
            bundle.description.sdp = *sdp;
        }
    }

    auto candidateEntries = extractJsonObjectArray(*response, "iceCandidates");
    for (const auto& entry : candidateEntries) {
        vic::transport::IceCandidate candidate{};
        candidate.candidate = extractJsonField(entry, "candidate").value_or("");
        candidate.sdpMid = extractJsonField(entry, "sdpMid").value_or("");
        if (auto idx = extractJsonInt(entry, "sdpMLineIndex")) {
            candidate.sdpMLineIndex = *idx;
        }
        bundle.iceCandidates.push_back(std::move(candidate));
    }

    if (bundle.description.sdp.empty()) {
        return std::nullopt;
    }
    return bundle;
}

bool MatchmakerClient::submitViewerAnswer(const std::string& code, const vic::transport::AnswerBundle& bundle) {
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) {
        return false;
    }

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) {
        return false;
    }
    WinHttpSetTimeouts(session, 3000, 5000, 5000, 5000);

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') {
        path.pop_back();
    }
    path += L"/answer";

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    std::ostringstream body;
    body << '{';
    body << "\"code\":\"" << jsonEscape(code) << "\",";
    body << "\"answer\":{\"type\":\"" << jsonEscape(bundle.description.type) << "\",\"sdp\":\"" << jsonEscape(bundle.description.sdp) << "\"},";
    body << "\"iceCandidates\":[";
    for (size_t i = 0; i < bundle.iceCandidates.size(); ++i) {
        const auto& candidate = bundle.iceCandidates[i];
        body << "{\"candidate\":\"" << jsonEscape(candidate.candidate) << "\",\"sdpMid\":\"" << jsonEscape(candidate.sdpMid) << "\",\"sdpMLineIndex\":" << candidate.sdpMLineIndex << '}';
        if (i + 1 < bundle.iceCandidates.size()) {
            body << ',';
        }
    }
    body << "]}";
    logging::global().log(logging::Logger::Level::Debug, "Matchmaker submit answer payload: " + body.str());

    std::string payload = body.str();

    BOOL result = WinHttpSendRequest(request,
        L"Content-Type: application/json\r\n",
        -1,
        payload.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(payload.data()),
        payload.empty() ? 0 : static_cast<DWORD>(payload.size()),
        payload.empty() ? 0 : static_cast<DWORD>(payload.size()),
        0);

    bool success = false;
    if (result && WinHttpReceiveResponse(request, nullptr)) {
        success = true;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    return success;
}

std::optional<RegisterResult> MatchmakerClient::registerHostExtended(const vic::transport::ConnectionInfo& info) {
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) {
        // Intentar fallback si falla el parseo
        if (tryFallback()) {
            logging::global().log(logging::Logger::Level::Info, "registerHostExtended: URL parse failed, trying fallback");
            return registerHostExtended(info);
        }
        logging::global().log(logging::Logger::Level::Error, "Failed to parse matchmaker URL");
        return std::nullopt;
    }

    logging::global().log(logging::Logger::Level::Info, 
        "registerHostExtended: Connecting to " + wideToUtf8(effectiveUrl()));

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) {
        logging::global().log(logging::Logger::Level::Error, "registerHostExtended: WinHttpOpen failed");
        return std::nullopt;
    }
    WinHttpSetTimeouts(session, 5000, 10000, 10000, 10000);

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        // Intentar fallback si falla la conexión
        if (tryFallback()) {
            logging::global().log(logging::Logger::Level::Info, "registerHostExtended: Connection failed, trying fallback");
            return registerHostExtended(info);
        }
        logging::global().log(logging::Logger::Level::Error, "registerHostExtended: WinHttpConnect failed");
        return std::nullopt;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') path.pop_back();
    path += L"/register";

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::ostringstream body;
    body << '{';
    body << "\"code\":\"" << jsonEscape(info.code) << "\",";
    // Incluir companyCode y clientId
    if (!companyCode_.empty()) {
        body << "\"companyCode\":\"" << jsonEscape(companyCode_) << "\",";
    }
    if (!clientId_.empty()) {
        body << "\"clientId\":\"" << jsonEscape(clientId_) << "\",";
    }
    // Serial de disco para modo free
    if (!diskSerial_.empty()) {
        body << "\"diskSerial\":\"" << jsonEscape(diskSerial_) << "\",";
    }
    if (isService_) {
        body << "\"isService\":true,";
        if (!deviceName_.empty()) {
            body << "\"deviceName\":\"" << jsonEscape(deviceName_) << "\",";
        }
    }
    body << "\"offer\":{\"type\":\"" << jsonEscape(info.offer.type) << "\",\"sdp\":\"" << jsonEscape(info.offer.sdp) << "\"},";
    body << "\"iceCandidates\":[";
    for (size_t i = 0; i < info.iceCandidates.size(); ++i) {
        const auto& candidate = info.iceCandidates[i];
        body << "{\"candidate\":\"" << jsonEscape(candidate.candidate) << "\",\"sdpMid\":\"" << jsonEscape(candidate.sdpMid) << "\",\"sdpMLineIndex\":" << candidate.sdpMLineIndex << '}';
        if (i + 1 < info.iceCandidates.size()) body << ',';
    }
    body << "],\"iceServers\":[";
    for (size_t i = 0; i < info.iceServers.size(); ++i) {
        const auto& server = info.iceServers[i];
        body << "{\"url\":\"" << jsonEscape(server.url) << "\"";
        if (server.username) body << ",\"username\":\"" << jsonEscape(*server.username) << "\"";
        if (server.credential) body << ",\"credential\":\"" << jsonEscape(*server.credential) << "\"";
        body << '}';
        if (i + 1 < info.iceServers.size()) body << ',';
    }
    body << "]}";
    
    std::string payload = body.str();
    logging::global().log(logging::Logger::Level::Debug, "Matchmaker registerExtended payload: " + payload);

    BOOL result = WinHttpSendRequest(request,
        L"Content-Type: application/json\r\n", -1,
        payload.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(payload.data()),
        static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0);

    RegisterResult regResult;
    if (result && WinHttpReceiveResponse(request, nullptr)) {
        auto response = readResponse(request);
        if (response) {
            if (auto code = extractJsonField(*response, "code")) regResult.code = *code;
            regResult.isFixedCode = response->find("\"isFixedCode\":true") != std::string::npos;
            regResult.emailSent = response->find("\"emailSent\":true") != std::string::npos;
            regResult.success = response->find("\"success\":true") != std::string::npos;
            
            // Extraer info de modo free/paid
            if (auto mode = extractJsonField(*response, "mode")) {
                regResult.mode = *mode;
            }
            if (auto maxMs = extractJsonInt(*response, "maxDurationMs")) {
                regResult.maxDurationMs = *maxMs;
            }
            if (auto maxMin = extractJsonInt(*response, "maxDurationMinutes")) {
                regResult.maxDurationMinutes = *maxMin;
            }
            
            logging::global().log(logging::Logger::Level::Info, 
                "registerHostExtended: Response success=" + std::string(regResult.success ? "true" : "false") + 
                ", code=" + regResult.code + ", mode=" + regResult.mode);
        }
    } else {
        DWORD error = GetLastError();
        logging::global().log(logging::Logger::Level::Error, 
            "registerHostExtended: HTTP request failed, error=" + std::to_string(error));
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        // Intentar fallback si falla el request HTTP
        if (tryFallback()) {
            logging::global().log(logging::Logger::Level::Info, "registerHostExtended: Request failed, trying fallback");
            return registerHostExtended(info);
        }
        return std::nullopt;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!regResult.success) {
        logging::global().log(logging::Logger::Level::Warning, 
            "registerHostExtended: Server returned success=false");
    }

    return regResult.success ? std::make_optional(regResult) : std::nullopt;
}

bool MatchmakerClient::preRegisterDevice(const std::string& code, const std::string& deviceName) {
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) return false;

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 5000, 10000, 10000, 10000);

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') path.pop_back();
    path += L"/api/devices/register";

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, 
        WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    // Construir JSON body
    std::ostringstream body;
    body << "{\"code\":\"" << jsonEscape(code) << "\"";
    if (!companyCode_.empty()) {
        body << ",\"clientId\":\"" << jsonEscape(companyCode_) << "\"";
    } else if (!clientId_.empty()) {
        body << ",\"clientId\":\"" << jsonEscape(clientId_) << "\"";
    }
    
    // Obtener nombre del equipo si no se proporcionó
    std::string devName = deviceName;
    if (devName.empty()) {
        char computerName[256] = {0};
        DWORD size = sizeof(computerName);
        if (GetComputerNameA(computerName, &size)) {
            devName = computerName;
        }
    }
    if (!devName.empty()) {
        body << ",\"deviceName\":\"" << jsonEscape(devName) << "\"";
    }
    body << "}";

    std::string payload = body.str();
    logging::global().log(logging::Logger::Level::Info, "Pre-registrando dispositivo: " + payload);

    bool success = false;
    if (WinHttpSendRequest(request, L"Content-Type: application/json\r\n", -1,
            const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()),
            static_cast<DWORD>(payload.size()), 0) && 
        WinHttpReceiveResponse(request, nullptr)) {
        
        auto response = readResponse(request);
        if (response) {
            success = response->find("\"success\":true") != std::string::npos;
            if (success) {
                logging::global().log(logging::Logger::Level::Info, "Dispositivo pre-registrado exitosamente: " + code);
            } else {
                logging::global().log(logging::Logger::Level::Warning, "Error pre-registrando dispositivo: " + *response);
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return success;
}

std::optional<std::string> MatchmakerClient::generateAvailableCode() {
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) return std::nullopt;

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session, 3000, 5000, 5000, 5000);

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') path.pop_back();
    path += L"/api/generate-code";

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::optional<std::string> code;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
        auto response = readResponse(request);
        if (response) {
            code = extractJsonField(*response, "code");
            if (code) {
                logging::global().log(logging::Logger::Level::Info, "Generated code from server: " + *code);
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return code;
}

bool MatchmakerClient::checkCodeAvailability(const std::string& code) {
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) return false;

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 3000, 5000, 5000, 5000);

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') path.pop_back();
    path += L"/api/check-code?code=";
    path += utf8ToWide(code);

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    bool available = false;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
        auto response = readResponse(request);
        if (response) {
            available = response->find("\"available\":true") != std::string::npos;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return available;
}

bool MatchmakerClient::sendHeartbeat(const std::string& code) {
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) return false;

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 2000, 3000, 3000, 3000);

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') path.pop_back();
    path += L"/heartbeat";

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    std::ostringstream body;
    body << "{\"code\":\"" << jsonEscape(code) << "\"";
    if (!clientId_.empty()) {
        body << ",\"clientId\":\"" << jsonEscape(clientId_) << "\"";
    }
    body << "}";
    std::string payload = body.str();

    bool success = false;
    BOOL result = WinHttpSendRequest(request,
        L"Content-Type: application/json\r\n", -1,
        const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0);
    
    if (result && WinHttpReceiveResponse(request, nullptr)) {
        success = true;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return success;
}

bool MatchmakerClient::disconnect(const std::string& code) {
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) return false;

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 2000, 3000, 3000, 3000);

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') path.pop_back();
    path += L"/disconnect";

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    std::ostringstream body;
    body << "{\"code\":\"" << jsonEscape(code) << "\"";
    if (!clientId_.empty()) {
        body << ",\"clientId\":\"" << jsonEscape(clientId_) << "\"";
    }
    body << "}";
    std::string payload = body.str();

    bool success = false;
    BOOL result = WinHttpSendRequest(request,
        L"Content-Type: application/json\r\n", -1,
        const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0);
    
    if (result && WinHttpReceiveResponse(request, nullptr)) {
        success = true;
        logging::global().log(logging::Logger::Level::Info, "Disconnected from server: " + code);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return success;
}

// ============== NUEVOS MÉTODOS PARA MODO FREE Y SERVICIO ==============

std::optional<AccountValidation> MatchmakerClient::validateAccount() {
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) {
        // Intentar fallback si falla el parseo
        if (tryFallback()) {
            return validateAccount();
        }
        return std::nullopt;
    }

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session, 3000, 5000, 5000, 5000);

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        // Intentar fallback si falla la conexión (DNS no resuelve, timeout, etc.)
        if (tryFallback()) {
            logging::global().log(logging::Logger::Level::Info, "Connection failed, trying fallback URL");
            return validateAccount();
        }
        return std::nullopt;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') path.pop_back();
    path += L"/api/validate-account";

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::ostringstream body;
    body << "{\"diskSerial\":\"" << jsonEscape(diskSerial_) << "\"";
    if (!companyCode_.empty()) {
        body << ",\"companyCode\":\"" << jsonEscape(companyCode_) << "\"";
    }
    body << "}";
    std::string payload = body.str();

    BOOL result = WinHttpSendRequest(request,
        L"Content-Type: application/json\r\n", -1,
        const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0);

    if (!result || !WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        // Intentar fallback si falla el envío/recepción (error de red, SSL, etc.)
        if (tryFallback()) {
            logging::global().log(logging::Logger::Level::Info, "HTTP request failed, trying fallback URL");
            return validateAccount();
        }
        return std::nullopt;
    }

    auto response = readResponse(request);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!response) return std::nullopt;

    // Si llegamos aquí, la conexión fue exitosa - loguear qué URL estamos usando
    logging::global().log(logging::Logger::Level::Info, 
        "Connected successfully to: " + wideToUtf8(effectiveUrl()));

    AccountValidation validation;
    
    auto mode = extractJsonField(*response, "mode");
    validation.isPaid = mode && *mode == "paid";
    
    auto allowed = extractJsonField(*response, "allowed");
    validation.allowed = allowed && *allowed == "true";
    
    if (auto waitMin = extractJsonInt(*response, "waitMinutes")) {
        validation.waitMinutes = *waitMin;
    }
    
    if (auto msg = extractJsonField(*response, "message")) {
        validation.message = *msg;
    }
    
    // Extraer info de usuario si es cuenta pagada
    if (auto userObj = extractJsonObject(*response, "user")) {
        if (auto name = extractJsonField(*userObj, "name")) {
            validation.userName = *name;
        }
        if (auto company = extractJsonField(*userObj, "companyName")) {
            validation.companyName = *company;
        }
    }

    logging::global().log(logging::Logger::Level::Info, 
        "Account validation: mode=" + (validation.isPaid ? std::string("paid") : std::string("free")) + 
        ", allowed=" + (validation.allowed ? "true" : "false"));

    return validation;
}

std::optional<ServicePasswordValidation> MatchmakerClient::validateServicePassword(const std::string& password) {
    // Intentar primero con URL actual, luego con fallback si falla
    bool triedFallback = false;
    
retry_with_fallback:
    
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) {
        if (!triedFallback && tryFallback()) {
            triedFallback = true;
            goto retry_with_fallback;
        }
        return std::nullopt;
    }

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) {
        return std::nullopt;
    }
    WinHttpSetTimeouts(session, 3000, 5000, 5000, 5000);

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        if (!triedFallback && tryFallback()) {
            triedFallback = true;
            goto retry_with_fallback;
        }
        return std::nullopt;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') path.pop_back();
    path += L"/api/validate-service-password";

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::ostringstream body;
    body << "{\"companyCode\":\"" << jsonEscape(companyCode_) << "\",";
    body << "\"servicePassword\":\"" << jsonEscape(password) << "\"}";
    std::string payload = body.str();

    BOOL result = WinHttpSendRequest(request,
        L"Content-Type: application/json\r\n", -1,
        const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0);
    
    if (!result) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (!triedFallback && tryFallback()) {
            triedFallback = true;
            goto retry_with_fallback;
        }
        return std::nullopt;
    }
    
    if (!WinHttpReceiveResponse(request, nullptr)) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        if (!triedFallback && tryFallback()) {
            triedFallback = true;
            goto retry_with_fallback;
        }
        return std::nullopt;
    }

    auto response = readResponse(request);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (!response) {
        return std::nullopt;
    }

    ServicePasswordValidation validation;
    
    // Usar extractJsonBool para valores booleanos (true/false sin comillas)
    auto valid = extractJsonBool(*response, "valid");
    validation.valid = valid.value_or(false);
    
    if (auto error = extractJsonField(*response, "error")) {
        validation.error = *error;
    }
    
    if (auto msg = extractJsonField(*response, "message")) {
        validation.message = *msg;
    }
    
    // El servidor devuelve userName y companyName en el root directamente
    if (auto name = extractJsonField(*response, "userName")) {
        validation.userName = *name;
    }
    if (auto company = extractJsonField(*response, "companyName")) {
        validation.companyName = *company;
    }
    
    // Fallback: también intentar buscar en objeto "user" anidado (compatibilidad)
    if (validation.userName.empty()) {
        if (auto userObj = extractJsonObject(*response, "user")) {
            if (auto name = extractJsonField(*userObj, "name")) {
                validation.userName = *name;
            }
            if (auto company = extractJsonField(*userObj, "companyName")) {
                validation.companyName = *company;
            }
        }
    }

    logging::global().log(logging::Logger::Level::Info, 
        "Service password validation: valid=" + (validation.valid ? std::string("true") : std::string("false")));

    return validation;
}

bool MatchmakerClient::endFreeSession() {
    if (diskSerial_.empty()) return false;
    
    auto parsed = parseUrl(effectiveUrl());
    if (!parsed) return false;

    HINTERNET session = WinHttpOpen(L"VicViewer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 2000, 3000, 3000, 3000);

    HINTERNET connect = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        return false;
    }

    std::wstring path = parsed->path;
    if (!path.empty() && path.back() == L'/') path.pop_back();
    path += L"/api/end-free-session";

    HINTERNET request = WinHttpOpenRequest(connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, parsed->secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    std::ostringstream body;
    body << "{\"diskSerial\":\"" << jsonEscape(diskSerial_) << "\"}";
    std::string payload = body.str();

    bool success = false;
    BOOL result = WinHttpSendRequest(request,
        L"Content-Type: application/json\r\n", -1,
        const_cast<char*>(payload.data()), static_cast<DWORD>(payload.size()), static_cast<DWORD>(payload.size()), 0);
    
    if (result && WinHttpReceiveResponse(request, nullptr)) {
        success = true;
        logging::global().log(logging::Logger::Level::Info, "Free session ended, cooldown started");
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return success;
}

} // namespace vic::matchmaking
