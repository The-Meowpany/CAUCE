# Verificacion completa CAUCE: firmware + backend + integracion E2E
$ErrorActionPreference = "Continue"
$root = Split-Path $PSScriptRoot -Parent
$fail = $false

# Toolchain MinGW-w64 requerido por el env 'native' (anteponer, no anexar)
$mingwBin = "C:\Users\filip\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
if (Test-Path "$mingwBin\g++.exe") {
    $env:Path = "$mingwBin;" + $env:Path
    $env:CC = "$mingwBin\gcc.exe"
    $env:CXX = "$mingwBin\g++.exe"
}

Write-Host "== 1/4 FIRMWARE: tests nativos ==" -ForegroundColor Cyan
pio test -e native --project-dir (Join-Path $root "firmware")
if ($LASTEXITCODE -ne 0) { $fail = $true }

Write-Host "== 2/4 FIRMWARE: build ESP32 ==" -ForegroundColor Cyan
pio run -e esp32dev --project-dir (Join-Path $root "firmware")
if ($LASTEXITCODE -ne 0) { $fail = $true }

Write-Host "== 3/4 BACKEND: pytest ==" -ForegroundColor Cyan
Push-Location (Join-Path $root "backend")
python -m pytest tests -q
if ($LASTEXITCODE -ne 0) { $fail = $true }
Pop-Location

Write-Host "== 4/4 INTEGRACION E2E nodo->servidor ==" -ForegroundColor Cyan
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run-e2e.ps1")
if ($LASTEXITCODE -ne 0) { $fail = $true }

if ($fail) {
    Write-Host "RESULTADO: FALLAS DETECTADAS" -ForegroundColor Red
    exit 1
}
Write-Host "RESULTADO: TODO OK (firmware 102 tests + ESP32 build + backend 26 tests + E2E)" -ForegroundColor Green
