#pragma once

#include <string>
#include <cstdint>

namespace vic {
namespace ui {

/**
 * Sistema anti-abuso para sesiones gratuitas.
 * Persiste datos en múltiples ubicaciones para evitar bypass por reinstalación.
 */
class AntiAbuse {
public:
    struct UsageData {
        uint32_t sessionCount = 0;      // Número de sesiones FREE usadas
        int64_t lastSessionEnd = 0;     // Timestamp de última sesión (segundos desde epoch)
        uint32_t currentWaitMinutes = 10; // Tiempo de espera actual en minutos
    };

    // Obtiene instancia singleton
    static AntiAbuse& instance();

    // Verifica si puede iniciar sesión FREE
    // Retorna true si puede, false si debe esperar
    bool canStartFreeSession();

    // Obtiene segundos restantes de espera (0 si puede conectar)
    int64_t getWaitSecondsRemaining();

    // Registra fin de sesión FREE (incrementa contador)
    void recordFreeSessionEnd();

    // Obtiene datos de uso actual
    UsageData getUsageData();

    // Obtiene mensaje formateado para mostrar al usuario
    std::wstring getWaitMessage();

private:
    AntiAbuse();
    ~AntiAbuse() = default;
    AntiAbuse(const AntiAbuse&) = delete;
    AntiAbuse& operator=(const AntiAbuse&) = delete;

    // Genera ID único de máquina basado en hardware
    std::string generateMachineId();

    // Calcula tiempo de espera según número de sesiones
    uint32_t calculateWaitMinutes(uint32_t sessionCount);

    // Guarda datos en todas las ubicaciones
    void saveData(const UsageData& data);

    // Carga datos (intenta todas las ubicaciones)
    UsageData loadData();

    // Ubicaciones de almacenamiento
    void saveToRegistry(const UsageData& data);
    void saveToAppData(const UsageData& data);
    void saveToProgramData(const UsageData& data);
    
    UsageData loadFromRegistry();
    UsageData loadFromAppData();
    UsageData loadFromProgramData();

    // Encriptación simple para ofuscar datos
    std::string encryptData(const std::string& data);
    std::string decryptData(const std::string& data);

    std::string machineId_;
    UsageData cachedData_;
    bool dataLoaded_ = false;
};

} // namespace ui
} // namespace vic
