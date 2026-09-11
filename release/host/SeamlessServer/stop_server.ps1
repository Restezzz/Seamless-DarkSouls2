# DS2 Seamless Co-op - stops the co-op server started by StartServer.bat.
# Остановка сервера Seamless Co-op.

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe  = Join-Path $here 'Server.exe'
$running = @(Get-Process -Name Server -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe })
if ($running.Count -gt 0) {
    $running | Stop-Process -Force
    Write-Host 'The server is stopped.' -ForegroundColor Green
    Write-Host '  Сервер остановлен.' -ForegroundColor DarkGray
} else {
    Write-Host 'The server is not running.'
    Write-Host '  Сервер не запущен.' -ForegroundColor DarkGray
}
