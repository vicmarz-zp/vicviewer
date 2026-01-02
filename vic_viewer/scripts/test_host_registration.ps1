param(
    [string]$MatchmakerUrl = "http://38.242.234.197:8787",
    [string]$HostAddress = "127.0.0.1",
    [int]$HostPort = 50050,
    [string]$SessionCode,
    [string]$Secret
)

$ErrorActionPreference = "Stop"
Write-Host "=== VicViewer Host Registration Test ===" -ForegroundColor Cyan
Write-Host "Matchmaker URL: $MatchmakerUrl" -ForegroundColor Gray

if (-not $SessionCode) {
    $SessionCode = [System.Guid]::NewGuid().ToString("N").Substring(0, 8).ToUpper()
    Write-Host "Generated session code: $SessionCode" -ForegroundColor Yellow
} else {
    $SessionCode = $SessionCode.Trim().ToUpper()
    Write-Host "Using provided session code: $SessionCode" -ForegroundColor Yellow
}

$headers = @{ "Content-Type" = "application/json" }
if ($Secret) {
    $headers["x-matchmaker-secret"] = $Secret
    Write-Host "Using shared secret authentication" -ForegroundColor Gray
}

$registerBody = @{ code = $SessionCode; address = $HostAddress; port = $HostPort } | ConvertTo-Json

try {
    Write-Host "\n→ Registering host..." -ForegroundColor Cyan
    $registerResponse = Invoke-RestMethod -Method Post -Uri "$MatchmakerUrl/register" -Headers $headers -Body $registerBody
    Write-Host "✅ Registration OK: $(ConvertTo-Json $registerResponse)" -ForegroundColor Green

    Write-Host "\n→ Resolving code..." -ForegroundColor Cyan
    $resolveResponse = Invoke-RestMethod -Method Get -Uri "$MatchmakerUrl/resolve?code=$SessionCode"
    Write-Host "✅ Resolve OK: $(ConvertTo-Json $resolveResponse)" -ForegroundColor Green

    Write-Host "\n→ Cleaning up registration..." -ForegroundColor Cyan
    Invoke-RestMethod -Method Delete -Uri "$MatchmakerUrl/register/$SessionCode" -Headers $headers | Out-Null
    Write-Host "✅ Session removed" -ForegroundColor Green

    Write-Host "\n🎯 Host registration workflow completed successfully." -ForegroundColor Green
    exit 0
}
catch {
    Write-Host "❌ Error: $($_.Exception.Message)" -ForegroundColor Red
    if ($_.Exception.Response) {
        try {
            $reader = New-Object System.IO.StreamReader($_.Exception.Response.GetResponseStream())
            $errorBody = $reader.ReadToEnd()
            if ($errorBody) {
                Write-Host "Server response:" -ForegroundColor Red
                Write-Host $errorBody -ForegroundColor DarkRed
            }
        } catch {}
    }
    exit 1
}
