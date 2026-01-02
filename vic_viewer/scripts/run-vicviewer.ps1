param(
    [ValidateSet("Win32", "x64")] [string]$Architecture = "Win32",
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$vicViewerRoot = Join-Path $repoRoot "vicviewer"
$buildDir = Join-Path $vicViewerRoot "build"
$exePath = Join-Path $buildDir "app" "VicViewer.exe"

if ($Rebuild.IsPresent -or -not (Test-Path $exePath)) {
    & (Join-Path $repoRoot "scripts" "bootstrap.ps1") -Architecture $Architecture
}

if (-not (Test-Path $exePath)) {
    throw "VicViewer executable not found at $exePath"
}

Write-Host "Launching VicViewer ($Architecture)" -ForegroundColor Green
& $exePath
