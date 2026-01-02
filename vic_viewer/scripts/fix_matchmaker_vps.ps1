# Diagnóstico y reparación del Matchmaker VPS
# Ejecutar: ssh root@38.242.234.197 'bash -s' < diagnose_matchmaker_vps.sh

Write-Host "=== Conectando al VPS para diagnosticar matchmaker ===" -ForegroundColor Cyan

$scriptPath = "C:\vic_viewer\scripts\diagnose_matchmaker_vps.sh"
if (-not (Test-Path $scriptPath)) {
    Write-Error "Script de diagnóstico no encontrado: $scriptPath"
    exit 1
}

try {
    Write-Host "Enviando script de diagnóstico al VPS..." -ForegroundColor Yellow
    $result = ssh root@38.242.234.197 'bash -s' < $scriptPath
    
    Write-Host "`n=== Resultado del diagnóstico ===" -ForegroundColor Green
    Write-Host $result
    
    Write-Host "`n=== Verificando conectividad post-diagnóstico ===" -ForegroundColor Cyan
    Start-Sleep -Seconds 2
    
    try {
        $response = Invoke-WebRequest -Uri "http://38.242.234.197:8787/health" -TimeoutSec 10 -UseBasicParsing
        Write-Host "✅ Matchmaker ahora responde: HTTP $($response.StatusCode)" -ForegroundColor Green
        Write-Host "Response: $($response.Content)" -ForegroundColor Gray
    } catch {
        Write-Host "❌ Matchmaker aún no responde: $($_.Exception.Message)" -ForegroundColor Red
        
        # Intento manual adicional
        Write-Host "`nIntentando arranque manual..." -ForegroundColor Yellow
        $manualStart = ssh root@38.242.234.197 "cd /opt/vicviewer-matchmaker && nohup node server.js > matchmaker.log 2>&1 &"
        Write-Host $manualStart
        
        Start-Sleep -Seconds 3
        try {
            $response2 = Invoke-WebRequest -Uri "http://38.242.234.197:8787/health" -TimeoutSec 10 -UseBasicParsing
            Write-Host "✅ Matchmaker arrancado manualmente: HTTP $($response2.StatusCode)" -ForegroundColor Green
        } catch {
            Write-Host "❌ Arranque manual también falló" -ForegroundColor Red
        }
    }
    
} catch {
    Write-Error "Error conectando al VPS: $_"
    Write-Host "`nSi no tienes SSH configurado, ejecuta manualmente en el VPS:" -ForegroundColor Yellow
    Write-Host "systemctl status vicviewer-matchmaker" -ForegroundColor White
    Write-Host "systemctl start vicviewer-matchmaker" -ForegroundColor White
    Write-Host "journalctl -u vicviewer-matchmaker --lines=20" -ForegroundColor White
}

Write-Host "`nPresiona Enter para continuar..." -ForegroundColor Gray
Read-Host