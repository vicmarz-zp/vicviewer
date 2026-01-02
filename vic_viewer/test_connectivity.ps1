# VicViewer Remote Server Connectivity Test
Write-Host "=== VicViewer Remote Server Connectivity Test ===" -ForegroundColor Cyan

# Test Matchmaker
Write-Host "`n1. Testing Matchmaker (38.242.234.197:8787)..." -ForegroundColor Yellow

try {
    $response = Invoke-WebRequest -Uri "http://38.242.234.197:8787/health" -TimeoutSec 10
    $status = $response.StatusCode
    $content = $response.Content
    
    if ($status -eq 200) {
        Write-Host "✅ Matchmaker Status: CONNECTED" -ForegroundColor Green
        Write-Host "Response: $content" -ForegroundColor Gray
    } else {
        Write-Host "❌ Matchmaker Status: HTTP $status" -ForegroundColor Red
    }
} catch {
    Write-Host "❌ Matchmaker Status: FAILED - $($_.Exception.Message)" -ForegroundColor Red
}

# Test TURN Server (basic TCP connectivity)
Write-Host "`n2. Testing TURN Server (38.242.234.197:3479)..." -ForegroundColor Yellow

try {
    $tcpClient = New-Object System.Net.Sockets.TcpClient
    $asyncResult = $tcpClient.BeginConnect("38.242.234.197", 3479, $null, $null)
    $waitResult = $asyncResult.AsyncWaitHandle.WaitOne(5000) # 5 second timeout
    
    if ($waitResult) {
        $tcpClient.EndConnect($asyncResult)
        Write-Host "✅ TURN Server TCP: REACHABLE" -ForegroundColor Green
        $tcpClient.Close()
    } else {
        Write-Host "❌ TURN Server TCP: TIMEOUT" -ForegroundColor Red
        $tcpClient.Close()
    }
} catch {
    Write-Host "❌ TURN Server TCP: FAILED - $($_.Exception.Message)" -ForegroundColor Red
}

# Test UDP connectivity (simplified)
Write-Host "`n3. Testing UDP connectivity to TURN server..." -ForegroundColor Yellow
try {
    $udpClient = New-Object System.Net.Sockets.UdpClient
    $udpClient.Connect("38.242.234.197", 3479)
    Write-Host "✅ TURN Server UDP: CLIENT CONNECTED" -ForegroundColor Green
    $udpClient.Close()
} catch {
    Write-Host "❌ TURN Server UDP: FAILED - $($_.Exception.Message)" -ForegroundColor Red
}

# Test tunnel relay control port
Write-Host "`n4. Testing Tunnel Relay Control (38.242.234.197:9400)..." -ForegroundColor Yellow
try {
    $relayClient = New-Object System.Net.Sockets.TcpClient
    $asyncRelay = $relayClient.BeginConnect("38.242.234.197", 9400, $null, $null)
    if ($asyncRelay.AsyncWaitHandle.WaitOne(5000)) {
        $relayClient.EndConnect($asyncRelay)
        Write-Host "✅ Tunnel Control TCP: REACHABLE" -ForegroundColor Green
        $relayClient.Close()
    } else {
        Write-Host "❌ Tunnel Control TCP: TIMEOUT" -ForegroundColor Red
        $relayClient.Close()
    }
} catch {
    Write-Host "❌ Tunnel Control TCP: FAILED - $($_.Exception.Message)" -ForegroundColor Red
}

# Summary
Write-Host "`n=== Summary ===" -ForegroundColor Cyan
Write-Host "Matchmaker (38.242.234.197:8787): Testing HTTP endpoint" -ForegroundColor White
Write-Host "TURN Server (38.242.234.197:3479): Testing TCP/UDP connectivity" -ForegroundColor White
Write-Host "Tunnel Relay (38.242.234.197:9400): Testing TCP control port" -ForegroundColor White

Write-Host "`n🎯 If both services are reachable, VicViewer should work with remote infrastructure!" -ForegroundColor Green
Write-Host "Press Enter to exit..." -ForegroundColor Gray
Read-Host