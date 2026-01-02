#include <windows.h>
#include <iostream>
#include <fstream>
#include <string>

// Crear un ejecutable mínimo que solo pruebe inicialización de componentes críticos
int main() {
    // Crear archivo de debug directo
    std::ofstream debug("c:\\vic_viewer\\debug_startup.log");
    debug << "Debug startup iniciado\n";
    debug.flush();

    try {
        debug << "Intentando cargar librerías...\n";
        debug.flush();
        
        // Intentar cargar datachannel.dll
        HMODULE hDataChannel = LoadLibraryA("datachannel.dll");
        if (!hDataChannel) {
            debug << "ERROR: No se pudo cargar datachannel.dll - " << GetLastError() << "\n";
            debug.flush();
            return 1;
        }
        debug << "datachannel.dll cargado OK\n";
        debug.flush();

        // Intentar cargar OpenCV
        HMODULE hOpenCV = LoadLibraryA("opencv_core4.dll");
        if (!hOpenCV) {
            debug << "ERROR: No se pudo cargar opencv_core4.dll - " << GetLastError() << "\n";
            debug.flush();
            return 1;
        }
        debug << "opencv_core4.dll cargado OK\n";
        debug.flush();

        debug << "Todas las librerías críticas cargadas correctamente\n";
        debug.flush();

        // Limpiar
        FreeLibrary(hDataChannel);
        FreeLibrary(hOpenCV);

        debug << "Debug completado exitosamente\n";
        debug.close();
        return 0;

    } catch (const std::exception& ex) {
        debug << "EXCEPCIÓN: " << ex.what() << "\n";
        debug.close();
        return 1;
    } catch (...) {
        debug << "EXCEPCIÓN DESCONOCIDA\n";
        debug.close();
        return 1;
    }
}