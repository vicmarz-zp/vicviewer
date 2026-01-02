# Script PowerShell para reiniciar matchmaker en VPS
Write-Host "=== Reiniciando Matchmaker en VPS ===" -ForegroundColor Cyan

$scriptPath = "C:\vic_viewer\scripts\restart_matchmaker.sh"

try {
    Write-Host "Conectando al VPS y ejecutando script de reinicio..." -ForegroundColor Yellow
    
    # Ejecutar script de reinicio en el VPS
    $bashScript = Get-Content $scriptPath -Raw
    $output = $bashScript | ssh root@38.242.234.197 'bash -s'
    Write-Host $output
    
    # Esperar un momento y verificar conectividad
    Write-Host "`nEsperando 5 segundos para que arranque..." -ForegroundColor Yellow
    Start-Sleep -Seconds 5
    
    # Verificar que el matchmaker responde
    Write-Host "Verificando conectividad al matchmaker..." -ForegroundColor Cyan
    try {
        $response = Invoke-WebRequest -Uri "http://38.242.234.197:8787/health" -TimeoutSec 10 -UseBasicParsing
        Write-Host "✅ Matchmaker funcionando: HTTP $($response.StatusCode)" -ForegroundColor Green
        if ($response.Content) {
            Write-Host "Response: $($response.Content)" -ForegroundColor Gray
        }
    } catch {
        Write-Host "❌ Matchmaker aún no responde: $($_.Exception.Message)" -ForegroundColor Red
        
        # Intentar endpoint raíz si /health falla
        try {
            $response = Invoke-WebRequest -Uri "http://38.242.234.197:8787/" -TimeoutSec 5 -UseBasicParsing
            Write-Host "✅ Matchmaker responde en /: HTTP $($response.StatusCode)" -ForegroundColor Green
        } catch {
            Write-Host "❌ Matchmaker completamente inaccesible" -ForegroundColor Red
        }
    }
    
} catch {
    Write-Host "❌ Error conectando al VPS: $_" -ForegroundColor Red
    Write-Host "`nComandos manuales para ejecutar en el VPS:" -ForegroundColor Yellow
    Write-Host "ssh root@38.242.234.197" -ForegroundColor White
    Write-Host "systemctl restart vicviewer-matchmaker" -ForegroundColor White
    Write-Host "# O manualmente:" -ForegroundColor Gray
    Write-Host "cd /opt/vicviewer-matchmaker && nohup node server.js > matchmaker.log 2>&1 &" -ForegroundColor White
}

Write-Host "`nPresiona Enter para continuar..." -ForegroundColor Gray
Read-Host