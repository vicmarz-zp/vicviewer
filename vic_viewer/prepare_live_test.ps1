# VicViewer Live Testing Script
# Este script prepara la aplicación para pruebas en vivo

Write-Host "=== VicViewer Live Testing Setup ===" -ForegroundColor Cyan

# Configurar variables de entorno
$env:PATH += ";C:\Program Files\CMake\bin"
$VcpkgRoot = "C:\vcpkg"
$ProjectRoot = "C:\vic_viewer\vicviewer"
$BuildDir = "$ProjectRoot\build"

Write-Host "`n1. Verificando dependencias..." -ForegroundColor Yellow

# Verificar CMake
try {
    $cmakeVersion = cmake --version | Select-String "cmake version"
    Write-Host "✅ CMake: $cmakeVersion" -ForegroundColor Green
} catch {
    Write-Host "❌ CMake no encontrado" -ForegroundColor Red
    exit 1
}

# Verificar vcpkg
if (Test-Path "$VcpkgRoot\vcpkg.exe") {
    Write-Host "✅ vcpkg: Disponible" -ForegroundColor Green
} else {
    Write-Host "❌ vcpkg no encontrado" -ForegroundColor Red
    exit 1
}

Write-Host "`n2. Verificando conectividad remota..." -ForegroundColor Yellow

# Test rápido de conectividad
try {
    $response = Invoke-WebRequest -Uri "http://38.242.234.197:8787/health" -TimeoutSec 5
    if ($response.StatusCode -eq 200) {
        Write-Host "✅ Matchmaker: Conectado" -ForegroundColor Green
    }
} catch {
    Write-Host "⚠️  Matchmaker: No conectado (continuar de todas formas)" -ForegroundColor Yellow
}

Write-Host "`n3. Preparando directorio de build..." -ForegroundColor Yellow

# Limpiar y recrear build directory
# if (Test-Path $BuildDir) {
#     Remove-Item $BuildDir -Recurse -Force
#     Write-Host "✅ Directorio de build limpiado" -ForegroundColor Green
# }

if (-not (Test-Path $BuildDir)) {
    New-Item -ItemType Directory -Path $BuildDir -Force | Out-Null
    Write-Host "✅ Directorio de build creado" -ForegroundColor Green
} else {
    Write-Host "✅ Directorio de build ya existe, omitiendo limpieza." -ForegroundColor Green
}

Write-Host "`n4. Configurando proyecto..." -ForegroundColor Yellow

# Cambiar al directorio de build
Set-Location $BuildDir

# Configurar con CMake
$cmakeArgs = @(
    "..",
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake"
)

try {
    & cmake @cmakeArgs
    if ($LASTEXITCODE -eq 0) {
        Write-Host "✅ Proyecto configurado exitosamente" -ForegroundColor Green
    } else {
        Write-Host "❌ Error en configuración de CMake" -ForegroundColor Red
        Write-Host "Revisa que todas las dependencias estén instaladas con vcpkg" -ForegroundColor Yellow
        exit 1
    }
} catch {
    Write-Host "❌ Error ejecutando CMake: $_" -ForegroundColor Red
    exit 1
}

Write-Host "`n5. Compilando aplicación..." -ForegroundColor Yellow

try {
    cmake --build . --config Release --parallel 4
    if ($LASTEXITCODE -eq 0) {
        Write-Host "✅ Compilación exitosa" -ForegroundColor Green
    } else {
        Write-Host "❌ Error en compilación" -ForegroundColor Red
        exit 1
    }
} catch {
    Write-Host "❌ Error durante compilación: $_" -ForegroundColor Red
    exit 1
}

Write-Host "`n6. Verificando ejecutables..." -ForegroundColor Yellow

$appPath = "$BuildDir\app\Release\VicViewer.exe"
$testPath = "$BuildDir\tests\Release"

if (Test-Path $appPath) {
    Write-Host "✅ VicViewer.exe: Encontrado" -ForegroundColor Green
} else {
    Write-Host "❌ VicViewer.exe: No encontrado" -ForegroundColor Red
}

Write-Host "`n=== ¡Listo para pruebas en vivo! ===" -ForegroundColor Green
Write-Host ""
Write-Host "Para ejecutar:" -ForegroundColor Cyan
Write-Host "  Host:   $appPath --mode=host --minimize" -ForegroundColor White
Write-Host "  Viewer: $appPath --mode=viewer --session-code=<CODIGO>" -ForegroundColor White
Write-Host ""
Write-Host "Para crear un paquete portable:" -ForegroundColor Cyan
Write-Host "  .\\scripts\\prepare_dist.ps1" -ForegroundColor White
Write-Host ""
Write-Host "Configuración remota:" -ForegroundColor Cyan
Write-Host "  Matchmaker: 38.242.234.197:8787" -ForegroundColor White
Write-Host "  TURN:       38.242.234.197:3479" -ForegroundColor White
Write-Host ""
Write-Host "¡La aplicación está configurada para conectarse automáticamente a los servidores remotos!" -ForegroundColor Green

# Volver al directorio original
Set-Location "C:\vic_viewer"

Write-Host "`nPresiona Enter para continuar..." -ForegroundColor Gray
Read-Host