param(
    [ValidateSet("Win32", "x64")] [string]$Architecture = "Win32",
    [string]$Generator = "Ninja",
    [switch]$SkipBuild,
    [switch]$RunTests
)

$ErrorActionPreference = "Stop"

# Determine repo root as parent directory of the scripts folder (where this script lives)
$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$vicViewerRoot = Join-Path $repoRoot "vicviewer"
$buildDir = Join-Path $vicViewerRoot "build"

if (-not (Test-Path $vicViewerRoot)) {
    throw "VicViewer source directory not found at $vicViewerRoot"
}

$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCmd) {
    $cmakeExe = $cmakeCmd.Source
} else {
    $cmakeExe = "C:\\Program Files\\CMake\\bin\\cmake.exe"
    if (-not (Test-Path $cmakeExe)) {
        throw "CMake executable not found. Install CMake or add it to PATH."
    }
}

$vcVarsFile = "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars"
$vcVarsFile += if ($Architecture -eq "x64") { "64.bat" } else { "32.bat" }
if (-not (Test-Path $vcVarsFile)) {
    Write-Warning "vcvars batch file not found at $vcVarsFile. Ensure Visual Studio Build Tools are installed."
    $vcVarsFile = $null
}

$configureArgs = @(
    "-S", "`"$vicViewerRoot`"",
    "-B", "`"$buildDir`""
)
if ($Generator) {
    $configureArgs += @("-G", "`"$Generator`"")
}
if ($Generator -match "Visual Studio") {
    $configureArgs += @("-A", $Architecture)
}

$vcpkgRoot = $env:VCPKG_ROOT
if ($vcpkgRoot) {
    $toolchainFile = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    if (Test-Path $toolchainFile) {
        $configureArgs += @("-DCMAKE_TOOLCHAIN_FILE=`"$toolchainFile`"")
        $triplet = if ($Architecture -eq "x64") { "x64-windows" } else { "x86-windows" }
        $configureArgs += @("-DVCPKG_TARGET_TRIPLET=$triplet")
    } else {
        Write-Warning "vcpkg toolchain not found at $toolchainFile"
    }
}

$commands = @()
if ($vcVarsFile) {
    $commands += "`"$vcVarsFile`""
}
$commands += "`"$cmakeExe`" $($configureArgs -join ' ')"
if (-not $SkipBuild.IsPresent) {
    $commands += "`"$cmakeExe`" --build `"$buildDir`""
}
if ($RunTests.IsPresent) {
    $commands += "ctest --test-dir `"$buildDir`" --output-on-failure"
}

$composed = $commands -join " && "
Write-Host "Executing: $composed" -ForegroundColor Cyan
cmd /c $composed
