# Builds the two release archives from release/host and release/joiner:
#   dist/Seamless-DS2-<version>-host.zip and dist/Seamless-DS2-<version>-joiner.zip
# The files sit at the root of each archive, ready to be extracted into the game
# folder. Text files get Windows line endings; per-host server data (keys,
# database, logs) is never packed. PowerShell 7.
param([string]$OutDir)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()
if (-not $OutDir) { $OutDir = Join-Path $root 'dist' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem

$textExtensions = @('.txt', '.ini', '.bat', '.ps1')
$neverShip = @('private.key', 'public.key', 'database.sqlite', 'last_activity.time', 'server_ip.txt')

function Set-WindowsLineEndings([string]$path) {
    $bytes = [IO.File]::ReadAllBytes($path)
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $start = 0
    if ($hasBom) { $start = 3 }
    $text = [Text.Encoding]::UTF8.GetString($bytes, $start, $bytes.Length - $start)
    $text = ($text -replace "`r`n", "`n") -replace "`n", "`r`n"
    [IO.File]::WriteAllText($path, $text, (New-Object Text.UTF8Encoding($hasBom)))
}

# The archives are built from release\host and release\joiner, so the DLL sitting
# in those folders is what ships -- copying the fresh build in was a manual step,
# and on 12.09 it was missed: the archives were packed with the previous build's
# DLL while their ini already said 0.1.3. The friend's download link would have
# carried a binary without any of the fixes. Not a manual step any more.
$built = Join-Path $root 'mod\build\bin\Release\dinput8.dll'
if (Test-Path -LiteralPath $built) {
    foreach ($kind in 'host', 'joiner') {
        Copy-Item -LiteralPath $built -Destination (Join-Path $root "release\$kind\dinput8.dll") -Force
    }
    $dll = Get-Item -LiteralPath $built
    'dinput8.dll  {0:N0} bytes, built {1:HH:mm:ss}' -f $dll.Length, $dll.LastWriteTime
} else {
    Write-Warning "no built DLL at $built -- packing whatever release\host and release\joiner already hold"
}

foreach ($kind in 'host', 'joiner') {
    $source = Join-Path $root "release\$kind"
    $stage = Join-Path ([IO.Path]::GetTempPath()) "seamless_package_$kind"
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    Copy-Item -LiteralPath $source -Destination $stage -Recurse

    Get-ChildItem -LiteralPath $stage -Recurse -File |
        Where-Object { $neverShip -contains $_.Name -or $_.FullName -match '\\logs\\' } |
        Remove-Item -Force
    Get-ChildItem -LiteralPath $stage -Recurse -File |
        Where-Object { $textExtensions -contains $_.Extension.ToLowerInvariant() } |
        ForEach-Object { Set-WindowsLineEndings $_.FullName }

    $zip = Join-Path $OutDir "Seamless-DS2-$version-$kind.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    [IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
    Remove-Item -LiteralPath $stage -Recurse -Force
    $item = Get-Item -LiteralPath $zip
    '{0}  {1:N2} MB' -f $item.Name, ($item.Length / 1MB)
}
