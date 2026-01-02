param(
    [int]$Port = 8787,
    [switch]$Install,
    [string]$Secret
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$serviceDir = Join-Path $repoRoot "matchmaker"

if (-not (Test-Path $serviceDir)) {
    throw "Matchmaker directory not found at $serviceDir"
}

Push-Location $serviceDir
try {
    if ($Install.IsPresent -or -not (Test-Path (Join-Path $serviceDir "node_modules"))) {
        Write-Host "Installing Node dependencies..." -ForegroundColor Cyan
        npm install
    }

    if ($Install.IsPresent -or -not (Test-Path (Join-Path $serviceDir "dist" "server.js"))) {
        Write-Host "Building TypeScript sources..." -ForegroundColor Cyan
        npm run build
    }

    $env:PORT = $Port
    if ($Secret) {
        $env:MATCHMAKER_SECRET = $Secret
    }

    Write-Host "Starting VicViewer matchmaker on port $Port" -ForegroundColor Green
    npm run start
} finally {
    Pop-Location
}
