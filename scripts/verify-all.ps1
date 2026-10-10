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
        ok    = 'RESULTADO: TODO OK (ESP32 build + E2E)'
    }
    en = @{
        step1 = '== 1/4 FIRMWARE: native tests =='
        step2 = '== 2/4 FIRMWARE: ESP32 build =='
        step3 = '== 3/4 BACKEND: pytest =='
        step4 = '== 4/4 E2E INTEGRATION node->server =='
        fail  = 'RESULT: FAILURES DETECTED'
        ok    = 'RESULT: ALL OK (ESP32 build + E2E)'
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
# The counts are captured, not just printed. The success line below used to hardcode them,
# which meant the number in the README and the number of tests that ran were two unrelated
# claims, and only the first was ever wrong quietly. Deriving the text from what actually
# ran means the figure in the release gate's output is the figure in the log.
$fwOut = pio test -e native --project-dir (Join-Path $root "firmware") 2>&1
$fwOut | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { $fail = $true }
$fwCount = 0
$fwSummary = ($fwOut | Select-String -Pattern '(\d+) test cases?: (\d+) succeeded' | Select-Object -Last 1)
if ($fwSummary) {
    $fwCount = [int]$fwSummary.Matches[0].Groups[1].Value
} elseif ($LASTEXITCODE -eq 0) {
    # A green run with no parseable count means the summary line changed shape. Saying so is
    # better than printing a total invented by this script.
    Write-Host "  note: could not read the firmware test count from the summary line" -ForegroundColor Yellow
}

Write-Host $T.step2 -ForegroundColor Cyan
pio run -e esp32dev --project-dir (Join-Path $root "firmware")
if ($LASTEXITCODE -ne 0) { $fail = $true }

Write-Host $T.step3 -ForegroundColor Cyan
# ruff runs here, and for the same reason CI runs it: it is the only check that sees the
# whole backend tree at once. It was missing from this script, so a lint error could pass a
# local `verify-all` and stop the pipeline - which is exactly what happened with an unused
# variable in a test. The version is pinned to CI's on purpose: a newer ruff finds more and a
# different version finds different things, so "it passed locally" has to mean something.
Write-Host "== 3a/4 RUFF ==" -ForegroundColor Cyan
# The console script, not `python -m ruff`: ruff is installed as a standalone executable and
# the module form does not exist, so a check that looked for the module reported "not
# installed" on a machine where ruff was present and working.
$ruffVer = "0.16.7"
$ruff = Get-Command ruff -ErrorAction SilentlyContinue
if (-not $ruff) {
    Write-Host "  ruff is not on PATH; install it with: pip install ruff==$ruffVer" -ForegroundColor Yellow
    $fail = $true
} else {
    & ruff --version | ForEach-Object { Write-Host "  $_" }
    Push-Location $root
    & ruff check backend simulator
    if ($LASTEXITCODE -ne 0) { $fail = $true }
    Pop-Location
}

Push-Location (Join-Path $root "backend")
$beOut = python -m pytest tests -q 2>&1
$beOut | ForEach-Object { Write-Host $_ }
$beExit = $LASTEXITCODE
Pop-Location
if ($beExit -ne 0) { $fail = $true }
$beCount = 0
$beSummary = ($beOut | Select-String -Pattern '(\d+) passed' | Select-Object -Last 1)
if ($beSummary) {
    $beCount = [int]$beSummary.Matches[0].Groups[1].Value
} elseif ($beExit -eq 0) {
    Write-Host "  note: could not read the backend test count from the summary line" -ForegroundColor Yellow
}

Write-Host $T.step4 -ForegroundColor Cyan
powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "run-e2e.ps1")
if ($LASTEXITCODE -ne 0) { $fail = $true }

if ($fail) {
    Write-Host $T.fail -ForegroundColor Red
    exit 1
}
Write-Host $T.ok -ForegroundColor Green
# Counted above from the runners' own summaries. If a count could not be read the line says
# `unknown` rather than a number this script made up.
$fwText = if ($fwCount -gt 0) { "$fwCount tests" } else { 'count unknown' }
$beText = if ($beCount -gt 0) { "$beCount tests" } else { 'count unknown' }
Write-Host ("  firmware {0}; backend {1}" -f $fwText, $beText) -ForegroundColor Green
