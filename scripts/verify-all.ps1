# CAUCE full verification: firmware + backend + E2E integration
$ErrorActionPreference = "Continue"
$root = Split-Path $PSScriptRoot -Parent
$fail = $false

$Lang = if ($env:CAUCE_LANG -match '^(?i)en') { 'en' } else { 'es' }
$T = @{
    es = @{
        step1 = '== 1/4 FIRMWARE: tests nativos =='
        step2 = '== 2/4 FIRMWARE: build ESP32 =='
        step3 = '== 3/4 BACKEND: pytest =='
        step4 = '== 4/4 INTEGRACION E2E nodo->servidor =='
        fail  = 'RESULTADO: FALLAS DETECTADAS'
        ok    = 'RESULTADO: TODO OK (firmware 264 tests + ESP32 build + backend 303 tests + E2E)'
    }
    en = @{
        step1 = '== 1/4 FIRMWARE: native tests =='
        step2 = '== 2/4 FIRMWARE: ESP32 build =='
        step3 = '== 3/4 BACKEND: pytest =='
        step4 = '== 4/4 E2E INTEGRATION node->server =='
        fail  = 'RESULT: FAILURES DETECTED'
        ok    = 'RESULT: ALL OK (firmware 264 tests + ESP32 build + backend 303 tests + E2E)'
    }
}[$Lang]

# MinGW-w64 toolchain required by the 'native' env (prepend, do not append)
$mingwBin = "C:\Users\filip\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin"
if (Test-Path "$mingwBin\g++.exe") {
    $env:Path = "$mingwBin;" + $env:Path
    $env:CC = "$mingwBin\gcc.exe"
    $env:CXX = "$mingwBin\g++.exe"
}

Write-Host $T.step1 -ForegroundColor Cyan
pio test -e native --project-dir (Join-Path $root "firmware")
if ($LASTEXITCODE -ne 0) { $fail = $true }

Write-Host $T.step2 -ForegroundColor Cyan
pio run -e esp32dev --project-dir (Join-Path $root "firmware")
if ($LASTEXITCODE -ne 0) { $fail = $true }

Write-Host $T.step3 -ForegroundColor Cyan
Push-Location (Join-Path $root "backend")
python -m pytest tests -q
if ($LASTEXITCODE -ne 0) { $fail = $true }
Pop-Location

Write-Host $T.step4 -ForegroundColor Cyan
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run-e2e.ps1")
if ($LASTEXITCODE -ne 0) { $fail = $true }

if ($fail) {
    Write-Host $T.fail -ForegroundColor Red
    exit 1
}
Write-Host $T.ok -ForegroundColor Green
