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
        ok    = 'RESULTADO: TODO OK (firmware 325 tests + ESP32 build + backend 501 tests + E2E)'
    }
    en = @{
        step1 = '== 1/4 FIRMWARE: native tests =='
        step2 = '== 2/4 FIRMWARE: ESP32 build =='
        step3 = '== 3/4 BACKEND: pytest =='
        step4 = '== 4/4 E2E INTEGRATION node->server =='
        fail  = 'RESULT: FAILURES DETECTED'
        ok    = 'RESULT: ALL OK (firmware 325 tests + ESP32 build + backend 501 tests + E2E)'
    }
}[$Lang]

# MinGW-w64 toolchain required by the 'native' env. Resolved, not hardcoded: a developer's
# absolute path in tracked source leaks a username and is wrong on every other machine.
# Set CAUCE_MINGW_BIN to override.
#
# The search looks for g++.exe rather than for a package name matching "mingw": the
# WinLibs package that ships MinGW-w64 is named after WinLibs, so a name filter finds
# nothing on the machine most likely to need this.
function Resolve-MingwBin {
    if ($env:CAUCE_MINGW_BIN) { return $env:CAUCE_MINGW_BIN }
    if (Get-Command g++ -ErrorAction SilentlyContinue) { return $null }
    $roots = @(
        (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'),
        (Join-Path $env:LOCALAPPDATA 'Programs'),
        (Join-Path $env:ProgramFiles 'mingw64'),
        (Join-Path $env:ProgramFiles 'Git\mingw64')
    )
    foreach ($root in $roots) {
        if (-not $root -or -not (Test-Path $root)) { continue }
        $hit = Get-ChildItem $root -Filter 'g++.exe' -Recurse -Depth 3 -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($hit) { return $hit.DirectoryName }
    }
    return $null
}

$mingwBin = Resolve-MingwBin
if ($mingwBin) {
    if (-not (Test-Path (Join-Path $mingwBin 'g++.exe'))) {
        throw "CAUCE_MINGW_BIN does not contain g++.exe: $mingwBin"
    }
    $env:Path = "$mingwBin;" + $env:Path
    $env:CC = Join-Path $mingwBin 'gcc.exe'
    $env:CXX = Join-Path $mingwBin 'g++.exe'
} elseif (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    throw "No MinGW-w64 g++ found. Install one, put it on PATH, or set CAUCE_MINGW_BIN."
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
