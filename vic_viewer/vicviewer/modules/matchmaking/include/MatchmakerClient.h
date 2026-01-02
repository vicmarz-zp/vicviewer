#pragma once

#include "Transport.h"

#include <optional>
#include <string>

namespace vic::matchmaking {

// Resultado de registro para servicios
struct RegisterResult {
    std::string code;
    bool isFixedCode = false;
    bool emailSent = false;
    bool success = false;
    std::string mode;  // "paid" or "free"
    int maxDurationMs = 0;  // Solo para modo free
    int maxDurationMinutes = 0;
};

// Resultado de validación de cuenta
struct AccountValidation {
    bool allowed = false;
    bool isPaid = false;
    int waitMinutes = 0;  // Minutos de espera si hay cooldown
    std::string message;
    std::string userName;
    std::string companyName;
};

// Resultado de validación de contraseña de servicio
struct ServicePasswordValidation {
    bool valid = false;
    std::string error;
    std::string message;
    std::string userName;
    std::string companyName;
};

class MatchmakerClient {
public:
    static constexpr const wchar_t* kDefaultServiceUrl = L"https://vicviewer.com";
    static constexpr const wchar_t* kFallbackServiceUrl = L"http://38.242.234.197:8080";
    explicit MatchmakerClient(std::wstring serviceUrl);

    // Setear el company_code extraído del nombre del ejecutable
    void setCompanyCode(const std::string& code) { companyCode_ = code; }
    const std::string& companyCode() const { return companyCode_; }
    
    // Setear el clientId (legacy, alias de companyCode)
    void setClientId(const std::string& clientId) { clientId_ = clientId; companyCode_ = clientId; }
    const std::string& clientId() const { return clientId_; }
    
    // Serial del disco duro físico (para modo free)
    void setDiskSerial(const std::string& serial) { diskSerial_ = serial; }
    const std::string& diskSerial() const { return diskSerial_; }
    
    // Setear modo servicio (código fijo persistente)
    void setServiceMode(bool isService) { isService_ = isService; }
    bool isServiceMode() const { return isService_; }
    
    // Setear nombre del dispositivo
    void setDeviceName(const std::string& name) { deviceName_ = name; }

    // Pre-registrar dispositivo (antes de instalar servicio)
    bool preRegisterDevice(const std::string& code, const std::string& deviceName = "");

    // Registro de host (retorna código asignado)
    std::optional<std::string> registerHost(const vic::transport::ConnectionInfo& info);
    
    // Registro extendido para servicios (incluye info de modo free/paid)
    std::optional<RegisterResult> registerHostExtended(const vic::transport::ConnectionInfo& info);
    
    // Validar cuenta antes de iniciar sesión (verifica si es pagada o modo free)
    std::optional<AccountValidation> validateAccount();
    
    // Validar contraseña de servicio (para pestaña Servicio)
    std::optional<ServicePasswordValidation> validateServicePassword(const std::string& password);
    
    // Notificar fin de sesión FREE (para registrar cooldown)
    bool endFreeSession();
    
    // Resolver código a conexión
    std::optional<vic::transport::ConnectionInfo> resolveCode(const std::string& code);
    
    // WebRTC answer exchange
    std::optional<vic::transport::AnswerBundle> fetchViewerAnswer(const std::string& code);
    bool submitViewerAnswer(const std::string& code, const vic::transport::AnswerBundle& bundle);
    
    // Generar código disponible desde el servidor
    std::optional<std::string> generateAvailableCode();
    
    // Verificar si un código está disponible
    bool checkCodeAvailability(const std::string& code);
    
    // Heartbeat para mantener vivo el código
    bool sendHeartbeat(const std::string& code);
    
    // Desconexión limpia
    bool disconnect(const std::string& code);
    
    // Obtener la URL actualmente en uso (principal o fallback)
    const std::wstring& currentUrl() const;

private:
    std::wstring serviceUrl_;
    std::wstring fallbackUrl_{kFallbackServiceUrl};  // URL de fallback si el dominio no resuelve
    bool usingFallback_ = false;  // Si estamos usando el fallback
    std::string clientId_;      // ID del cliente para notificación por email (legacy)
    std::string companyCode_;   // Código de empresa (4 caracteres)
    std::string diskSerial_;    // Serial del disco duro para modo free
    std::string deviceName_;    // Nombre del dispositivo
    bool isService_ = false;    // Modo servicio (código fijo)
    
    // Obtener la URL efectiva (principal o fallback)
    const std::wstring& effectiveUrl() const;
    // Intentar cambiar a fallback si hay error de conexión
    bool tryFallback();
};

} // namespace vic::matchmaking
