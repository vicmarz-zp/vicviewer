# VicViewer Quick Launch Scripts
# Ejecutar desde C:\vic_viewer

param(
    [Parameter(Mandatory=$true)]
    [ValidateSet("host", "viewer", "both")]
    [string]$Mode,

    [string]$SessionCode
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$candidatePaths = @(
    (Join-Path $scriptDir "VicViewer.exe"),
    (Join-Path $scriptDir "app\Release\VicViewer.exe"),
    (Join-Path $scriptDir "vicviewer\build\app\Release\VicViewer.exe"),
    (Join-Path $scriptDir "build\app\Release\VicViewer.exe"),
    "C:\vic_viewer\vicviewer\build\app\Release\VicViewer.exe"
)

$AppPath = $candidatePaths | Where-Object { Test-Path $_ } | Select-Object -First 1

Write-Host "=== VicViewer Live Test Launcher ===" -ForegroundColor Cyan

if (-not $env:VIC_RELAY_HOST) {
    $env:VIC_RELAY_HOST = "38.242.234.197"
}
if (-not $env:VIC_RELAY_CONTROL_PORT) {
    $env:VIC_RELAY_CONTROL_PORT = "9400"
}
if (-not $env:VIC_RELAY_DATA_PORT) {
    $env:VIC_RELAY_DATA_PORT = "9401"
}

if (-not $env:VIC_TUNNEL_HOST) {
    $env:VIC_TUNNEL_HOST = $env:VIC_RELAY_HOST
}
if (-not $env:VIC_TUNNEL_CONTROL_PORT) {
    $env:VIC_TUNNEL_CONTROL_PORT = $env:VIC_RELAY_CONTROL_PORT
}
if (-not $env:VIC_TUNNEL_DATA_PORT) {
    $env:VIC_TUNNEL_DATA_PORT = $env:VIC_RELAY_DATA_PORT
}

# Verificar que la aplicación existe
if (-not $AppPath -or -not (Test-Path $AppPath)) {
    if ($AppPath) {
        Write-Host "❌ VicViewer.exe no encontrado en: $AppPath" -ForegroundColor Red
    } else {
        Write-Host "❌ VicViewer.exe no encontrado. Ejecuta el script desde la carpeta del proyecto o desde el paquete generado." -ForegroundColor Red
    }
    Write-Host "Ejecuta primero: .\prepare_live_test.ps1" -ForegroundColor Yellow
    exit 1
}

Write-Host "✅ Aplicación encontrada: $AppPath" -ForegroundColor Green

switch ($Mode) {
    "host" {
        Write-Host "`n🖥️  Lanzando VicViewer en modo HOST..." -ForegroundColor Green
    Write-Host "La aplicación se registrará automáticamente en el matchmaker remoto" -ForegroundColor Gray
    Write-Host "Matchmaker: http://38.242.234.197:8787" -ForegroundColor Gray
    Write-Host "Túnel: $($env:VIC_RELAY_HOST):$($env:VIC_RELAY_CONTROL_PORT)/$($env:VIC_RELAY_DATA_PORT)" -ForegroundColor Gray
        Write-Host ""
        $hostArgs = @("--mode=host", "--minimize")
        & $AppPath @hostArgs
    }
    
    "viewer" {
        Write-Host "`n👁️  Lanzando VicViewer en modo VIEWER..." -ForegroundColor Blue
        Write-Host "Ingresa el código de sesión proporcionado por el host" -ForegroundColor Gray
    Write-Host "La aplicación se conectará automáticamente al matchmaker y túnel remoto" -ForegroundColor Gray
        Write-Host ""
        $viewerArgs = @("--mode=viewer")
        if ($SessionCode) {
            $viewerArgs += "--session-code=$SessionCode"
        }
        & $AppPath @viewerArgs
    }
    
    "both" {
        Write-Host "`n🔄 Lanzando ambas instancias..." -ForegroundColor Magenta
        Write-Host "Se abrirán dos ventanas: Host y Viewer" -ForegroundColor Gray
        Write-Host ""
        
        # Lanzar host en background
        $hostArgs = @("--mode=host", "--minimize")
        Start-Process -FilePath $AppPath -ArgumentList $hostArgs -WindowStyle Normal
        Start-Sleep 2
        
        # Lanzar viewer
        $viewerArgs = @("--mode=viewer")
        if ($SessionCode) {
            $viewerArgs += "--session-code=$SessionCode"
        }
        & $AppPath @viewerArgs
    }
}

Write-Host "`nPrueba completada." -ForegroundColor Green