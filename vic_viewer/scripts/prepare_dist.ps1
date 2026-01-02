param(
    [string]$Configuration = "Release",
    [string]$OutputRoot = "C:\vic_viewer\dist",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

Write-Host "=== VicViewer Distribution Prep ===" -ForegroundColor Cyan

$projectRoot = "C:\vic_viewer\vicviewer"
$buildDir = Join-Path $projectRoot "build"
$vcpkgRoot = "C:\vcpkg"
$cmakeExe = "C:\Program Files\CMake\bin\cmake.exe"

if (-not $SkipBuild) {
    Write-Host "Compilando configuración $Configuration..." -ForegroundColor Yellow
    if (-not (Test-Path $cmakeExe)) {
        throw "cmake.exe no encontrado en '$cmakeExe'."
    }
    & $cmakeExe --build $buildDir --config $Configuration --target ALL_BUILD | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "La compilación falló, revisa los logs anteriores."
    }
} else {
    Write-Host "Compilación omitida por parámetro." -ForegroundColor Yellow
}

$exePath = Join-Path $buildDir "app\$Configuration\VicViewer.exe"
if (-not (Test-Path $exePath)) {
    throw "VicViewer.exe no encontrado en '$exePath'. Ejecuta prepare_live_test.ps1 primero."
}

if (-not (Test-Path $OutputRoot)) {
    New-Item -ItemType Directory -Path $OutputRoot | Out-Null
}

$distDir = Join-Path $OutputRoot "VicViewer_$Configuration"
if (Test-Path $distDir) {
    Remove-Item $distDir -Recurse -Force
}
New-Item -ItemType Directory -Path $distDir | Out-Null

Write-Host "Copiando ejecutable principal..." -ForegroundColor Yellow
Copy-Item $exePath $distDir -Force

$dependenciesCopied = 0
$vcpkgBin = Join-Path $vcpkgRoot "installed\x64-windows\bin"
if (Test-Path $vcpkgBin) {
    Write-Host "Copiando dependencias de vcpkg..." -ForegroundColor Yellow
    Get-ChildItem -Path $vcpkgBin -Filter *.dll | ForEach-Object {
        Copy-Item $_.FullName -Destination $distDir -Force
        $dependenciesCopied++
    }
    Write-Host "  → DLLs copiadas: $dependenciesCopied" -ForegroundColor Green
} else {
    Write-Warning "No se encontró '$vcpkgBin'. Asegúrate de que vcpkg esté instalado." 
}

$datachannelDll = Join-Path $buildDir "_deps\libdatachannel-build\Release\datachannel.dll"
if (Test-Path $datachannelDll) {
    Copy-Item $datachannelDll -Destination $distDir -Force
    Write-Host "Copiado datachannel.dll desde build/_deps" -ForegroundColor Green
} else {
    Write-Warning "No se encontró datachannel.dll en '$datachannelDll'."
}

$msvcCopied = $false
$redistRoots = @(
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Redist\MSVC"
) | Where-Object { Test-Path $_ }

if ($redistRoots.Count -gt 0) {
    $crtCandidates = foreach ($root in $redistRoots) {
        Get-ChildItem $root -Directory | ForEach-Object {
            $version = $null
            if ([version]::TryParse($_.Name, [ref]$version)) {
                $archRoot = Join-Path $_.FullName "x64"
                if (Test-Path $archRoot) {
                    Get-ChildItem $archRoot -Directory -Filter "Microsoft.VC*.CRT" | ForEach-Object {
                        [PSCustomObject]@{
                            Version = $version
                            Path    = $_.FullName
                        }
                    }
                }
            }
        }
    }

    $bestCrt = $crtCandidates | Sort-Object Version -Descending | Select-Object -First 1
    if ($bestCrt) {
        Write-Host "Copiando runtime de MSVC ($($bestCrt.Version))..." -ForegroundColor Yellow
        Copy-Item (Join-Path $bestCrt.Path "*.dll") -Destination $distDir -Force
        $msvcCopied = $true
    }
}

if (-not $msvcCopied) {
    Write-Warning "No se pudo localizar el runtime de MSVC. El paquete necesitará el 'Microsoft Visual C++ Redistributable 2022 x64' instalado manualmente en la máquina destino."
}

Write-Host "Copiando scripts de soporte..." -ForegroundColor Yellow
Copy-Item "C:\vic_viewer\launch_test.ps1" $distDir -Force
Copy-Item "C:\vic_viewer\test_connectivity.ps1" $distDir -Force
Copy-Item "C:\vic_viewer\scripts\test_host_registration.ps1" $distDir -Force
Copy-Item "C:\vic_viewer\LIVE_TESTING_GUIDE.md" $distDir -Force

$readmePath = Join-Path $distDir "README_DIST.md"
$readmeContent = @"
# Paquete VicViewer para pruebas en vivo

## Requisitos previos
1. Windows 10/11 x64 con escritorio activo.
2. Microsoft Visual C++ Redistributable 2022 x64 (ya incluido si ves los archivos `vcruntime*.dll`).
3. Conectividad saliente a `38.242.234.197` (HTTP 8787, UDP/TCP 3479).

## Pasos rápidos
1. Ejecuta `test_connectivity.ps1` para validar acceso a matchmaker/TURN.
2. Ejecuta `scripts/test_host_registration.ps1` para confirmar que el matchmaker acepta registros.
3. Inicia el host con `launch_test.ps1 -Mode host` (automáticamente define `VIC_RELAY_*`).
4. En otra máquina o sesión, ejecuta `launch_test.ps1 -Mode viewer -SessionCode <CÓDIGO>`.

## Contenido del paquete
- `VicViewer.exe` → Aplicación principal.
- `*.dll` → Dependencias de terceros (vcpkg) y runtime de MSVC (vcruntime/msvcp) listos para usar.
- `launch_test.ps1`, `test_connectivity.ps1`, `scripts/test_host_registration.ps1`, `LIVE_TESTING_GUIDE.md` → Scripts y guía de ejecución.

## Notas
- El host escucha en el puerto TCP 50050. Permite el tráfico en tu firewall local.
- Matchmaker, TURN y el túnel (`38.242.234.197:9400/9401`) están preconfigurados; no necesitas ajustes adicionales.
- Usa `launch_test.ps1 -Mode viewer -SessionCode <CÓDIGO>` para auto completar el código recibido del host.
"@
Set-Content -Path $readmePath -Value $readmeContent -Encoding UTF8

Write-Host "=== Paquete generado en: $distDir ===" -ForegroundColor Green
Write-Host "Archivos incluidos:" -ForegroundColor Cyan
Get-ChildItem $distDir | Select-Object Name, Length | Format-Table | Out-String | Write-Host
