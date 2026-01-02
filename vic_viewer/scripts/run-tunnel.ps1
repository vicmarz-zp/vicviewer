param(
    [string]$ControlPort = "9400",
    [string]$DataPort = "9401"
)

$ErrorActionPreference = "Stop"

Write-Host "=== VicViewer Tunnel Server ===" -ForegroundColor Cyan
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$tunnelDir = Join-Path (Split-Path -Parent $scriptRoot) "tunnel"

if (-not (Test-Path (Join-Path $tunnelDir "package.json"))) {
    throw "No se encontró la carpeta 'tunnel' en $tunnelDir"
}

Push-Location $tunnelDir

if (-not (Test-Path "node_modules")) {
    Write-Host "Instalando dependencias..." -ForegroundColor Yellow
    npm install | Out-Null
}

$env:VIC_TUNNEL_CONTROL_PORT = $ControlPort
$env:VIC_TUNNEL_DATA_PORT = $DataPort

Write-Host "Iniciando servidor (control=$ControlPort, data=$DataPort)..." -ForegroundColor Green
npm start

Pop-Location
