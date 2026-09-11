# DS2 Seamless Co-op - starts the co-op server for the host.
# Запуск сервера Seamless Co-op на компьютере хоста.
#
# 1. Finds the host's VPN address (Radmin VPN 26.x, then Hamachi 25.x) or takes
#    it from server_ip.txt next to this script, and writes it into the server's
#    config: it is the address the friends' games are sent to.
# 2. Starts Server.exe hidden (with a console window of its own it closes) and
#    keeps its output in logs\.
# 3. Copies the server's public key into the game folder: the host's game reads
#    it there, and it is the file to send to friends.
param([switch]$DryRun)

$ErrorActionPreference = 'Stop'
$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$game    = Split-Path -Parent $here
$exe     = Join-Path $here 'Server.exe'
$cfg     = Join-Path $here 'Saved\default\config.json'
$key     = Join-Path $here 'Saved\default\public.key'
$gameKey = Join-Path $game 'ds2_server_public.key'
$loginPort = 50031

function Say([string]$en, [string]$ru, [ConsoleColor]$color = 'Gray') {
    Write-Host $en -ForegroundColor $color
    Write-Host ('  ' + $ru) -ForegroundColor DarkGray
}

if (-not (Test-Path -LiteralPath $exe) -or -not (Test-Path -LiteralPath $cfg)) {
    Say "Server.exe or its config is missing in $here" "Не найден Server.exe или его настройки в $here" Red
    exit 1
}

# The address friends connect to.
$ip = $null
$override = Join-Path $here 'server_ip.txt'
if (Test-Path -LiteralPath $override) { $ip = (Get-Content -LiteralPath $override -Raw).Trim() }
if (-not $ip) {
    $all = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | ForEach-Object { $_.IPAddress })
    $ip = $all | Where-Object { $_ -like '26.*' } | Select-Object -First 1
    if (-not $ip) { $ip = $all | Where-Object { $_ -like '25.*' } | Select-Object -First 1 }
}
if (-not $ip) {
    Say 'No Radmin VPN (26.x.x.x) or Hamachi (25.x.x.x) address on this PC.' `
        'На этом компьютере нет адреса Radmin VPN (26.x.x.x) или Hamachi (25.x.x.x).' Yellow
    Say 'Turn Radmin VPN on and run StartServer.bat again, or write your address into SeamlessServer\server_ip.txt.' `
        'Включи Radmin VPN и запусти StartServer.bat снова или впиши свой адрес в SeamlessServer\server_ip.txt.' Yellow
    exit 1
}

$json = [IO.File]::ReadAllText($cfg)
$json = $json -replace '("ServerHostname":\s*)"[^"]*"', ('${1}"' + $ip + '"')
$json = $json -replace '("ServerPrivateHostname":\s*)"[^"]*"', ('${1}"' + $ip + '"')
if ($DryRun) {
    Say "[dry run] address: $ip" "[проверка] адрес: $ip" Cyan
    ($json -split "`n") | Where-Object { $_ -match 'Server(Private)?Hostname' } | ForEach-Object { Write-Host $_.Trim() }
    Say "[dry run] key would be copied to: $gameKey" "[проверка] ключ был бы скопирован в: $gameKey" Cyan
    exit 0
}
[IO.File]::WriteAllText($cfg, $json, (New-Object Text.UTF8Encoding($false)))

# A copy already running from this folder goes first.
Get-Process -Name Server -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force
Start-Sleep -Milliseconds 700

$logs = Join-Path $here 'logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
Start-Process -FilePath $exe -WorkingDirectory $here -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $logs 'server_out.txt') `
    -RedirectStandardError (Join-Path $logs 'server_err.txt')

# The first start makes the server's keys; then it listens for logins.
$up = $false
for ($i = 0; $i -lt 40 -and -not $up; $i++) {
    Start-Sleep -Milliseconds 500
    $up = [bool](Get-NetTCPConnection -LocalPort $loginPort -State Listen -ErrorAction SilentlyContinue)
}
if (Test-Path -LiteralPath $key) { Copy-Item -LiteralPath $key -Destination $gameKey -Force }

Write-Host ''
if (-not $up) {
    Say "The server did not start. Look at SeamlessServer\logs\server_out.txt" `
        "Сервер не запустился. Смотри SeamlessServer\logs\server_out.txt" Red
    exit 1
}
Say 'The server is running.' 'Сервер запущен.' Green
Say "Your address for friends: $ip" "Адрес для друзей: $ip" Green
if (Test-Path -LiteralPath $gameKey) {
    Say "Key file for friends (once; they put it into their game folder): $gameKey" `
        "Файл-ключ для друзей (один раз; кладётся в их папку игры): $gameKey"
}
Say 'Now start the game. To stop the server: StopServer.bat. Web panel: http://127.0.0.1:50005' `
    'Теперь запускай игру. Остановить сервер: StopServer.bat. Веб-панель: http://127.0.0.1:50005'
