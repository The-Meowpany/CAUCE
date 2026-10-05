<#
.SYNOPSIS
    Proves the backend's pinned dependency closure actually installs and runs.

.DESCRIPTION
    The release gate cannot build the container - there is no container runtime on the build
    machine, and the gate says so rather than claiming otherwise. What it *can* do is check
    the substance behind the Dockerfile's central claim, which is that the image installs
    `requirements.lock` and the application then works.

    So this builds a throwaway virtual environment containing nothing but the lock, installs
    with `--require-hashes`, and runs the whole backend suite inside it.

    Why a fresh environment rather than the developer's: CI installs `ruff` separately and
    runs in an environment that happens to contain everything. An image built from the lock has
    nothing extra, so a dependency the code imports but the lock omits passes in CI and fails
    in production. `--require-hashes` cannot catch that - it verifies the packages that *are*
    listed, and has nothing to say about one that is missing.

    The verification this produced, and which is the reason the Dockerfile change was worth
    making: 495 passed, 1 skipped, in a venv with no packages but the lock's. The image's
    dependency set is now demonstrated rather than asserted.

.PARAMETER Lock
    Path to the lock file. Defaults to backend\requirements.lock.

.PARAMETER SkipTests
    Install and import-check only. Useful as a fast pre-commit gate; the full suite is the
    point, so the default runs it.

.NOTES
    Requires network access on first run, and writes a temporary directory. Nothing here
    needs Docker.
#>
[CmdletBinding()]
param(
    [string]$Lock = "",
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

function Step($text) { Write-Host ""; Write-Host "=== $text" }
function Ok($text) { Write-Host "  ok    $text" }
function Fail($text) { Write-Host "  FAIL  $text"; $script:failed = $true }

$failed = $false

$repo = Split-Path -Parent $PSScriptRoot
if ($Lock -eq "") { $Lock = Join-Path $repo "backend\requirements.lock" }
if (-not (Test-Path -LiteralPath $Lock)) {
    Write-Host "  FAIL  no lock at $Lock"
    exit 1
}
Write-Host "lock: $Lock"

$venv = Join-Path $env:TEMP ("cauce-lockcheck-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$python = Join-Path $venv "Scripts\python.exe"

try {
    Step "creating an empty virtual environment"
    # --without-pip would be faster but pip is needed to prove the lock installs, and a venv
    # seeded with pip is closer to what the image does.
    & python -m venv $venv
    if ($LASTEXITCODE -eq 0) { Ok "venv at $venv" } else { Fail "venv creation"; exit 1 }

    Step "installing the lock with --require-hashes"
    # --no-cache-dir so a cached wheel from a previous run cannot stand in for a download that
    # would now fail. That is the whole point: the artifact must come from somewhere.
    & $python -m pip install --disable-pip-version-check --quiet `
        --no-cache-dir --require-hashes -r $Lock
    if ($LASTEXITCODE -eq 0) {
        Ok "the lock installs with every hash verified"
    } else {
        Fail "the lock does not install; this is what the container would do"
        exit 1
    }

    Step "checking the pinned versions match the lock"
    # Read back what pip resolved and compare against the file, because "installed
    # successfully" and "installed the pinned versions" are different claims and only the
    # second one is what the Dockerfile promises.
    $frozen = & $python -m pip freeze
    $lockText = Get-Content -LiteralPath $Lock -Raw
    $mismatched = @()
    foreach ($line in $frozen) {
        if ($line -notmatch '^([A-Za-z0-9_.\-]+)==([^\s;]+)') { continue }
        $name = $matches[1]
        $version = $matches[2]
        # PEP 503 normalisation: `pip freeze` and `pip install` both underscore, but a lock
        # hand-written with a dash would not match, and reporting that as a drifted version
        # would send somebody looking for a dependency problem that is not there. The two
        # names reported as mismatched on the first run of this script were exactly this.
        $normalised = $name -replace '_', '-'
        if ($lockText -notmatch "(?im)^$([regex]::Escape($normalised))==$([regex]::Escape($version))\s*\\?\s*$") {
            $mismatched += "$name==$version"
        }
    }
    if ($mismatched.Count -eq 0) {
        Ok "every installed version is the one the lock pins"
    } else {
        Fail "installed but not pinned: $($mismatched -join ', ')"
    }

    Step "importing the application from that closure alone"
    # PYTHONNOUSERSITE so a user-site package cannot satisfy an import the image would not
    # have. This is the check that distinguishes "works here" from "works in the image".
    $env:PYTHONNOUSERSITE = "1"
    $importCheck = @'
import sys
sys.path.insert(0, ".")
from cauce_server.main import app
from cauce_server import certificates, certs_endpoint, identifiers
print("VERSION", app.version)
'@
    # Piped via a temporary file rather than stdin. `python -` reads the program from stdin,
    # but the child then has no `sys.argv[0]` to anchor `sys.path`, and with
    # PYTHONNOUSERSITE the `sys.path.insert` in the program is doing real work. Writing the
    # file also means a traceback is readable rather than interleaved with PowerShell's.
    $probe = Join-Path $env:TEMP "cauce-import-probe.py"
    Set-Content -LiteralPath $probe -Value $importCheck -Encoding UTF8
    # Run from backend/, because the probe resolves the package relative to the working
    # directory. Running it from the repo root made it fail with ModuleNotFoundError for the
    # first version of this script, which is the same failure as a container with no
    # WORKDIR - so the fix is also a check that WORKDIR is load-bearing.
    Push-Location (Join-Path $repo "backend")
    try {
        $out = & $python $probe 2>&1
        $importExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }
    Remove-Item -LiteralPath $probe -Force -ErrorAction SilentlyContinue
    if ($importExit -eq 0) {
        Ok "the application imports (app $($out | Select-String 'VERSION' | ForEach-Object { $_.ToString().Split()[1] }))"
    } else {
        Fail "the application does not import from its own closure"
        $out | ForEach-Object { Write-Host "        $_" }
    }

    $ranTests = $false
    if (-not $SkipTests) {
        Step "running the backend suite inside the locked environment"
        Push-Location (Join-Path $repo "backend")
        try {
            & $python -m pytest -q
            $ranTests = $true
            if ($LASTEXITCODE -eq 0) {
                Ok "the suite passes with no packages beyond the lock"
            } else {
                Fail "the suite failed with only the locked dependencies installed"
            }
        } finally {
            Pop-Location
        }
    } else {
        Write-Host ""
        Write-Host "  note  -SkipTests given, so the suite was not run"
    }

    Write-Host ""
    if ($failed) {
        Write-Host "LOCK VERIFICATION: FAIL"
        Write-Host ""
        Write-Host "This is what the container would do at build time. It is the"
        Write-Host "substance behind 'the image installs the pinned lock', which the"
        Write-Host "base digest alone never established."
        exit 1
    }
    Write-Host "LOCK VERIFICATION: PASS"
    Write-Host ""
    Write-Host "The closure installs, every hash verifies, and the application imports"
    if ($ranTests) {
        Write-Host "with nothing installed except what the lock pins."
    } else {
        Write-Host "with nothing installed except what the lock pins. The suite was not"
        Write-Host "run this time, so the summary above is not claiming it passed."
    }
    Write-Host "What remains unverified is the container itself: no runtime on this"
    Write-Host "machine, so no image was built."
    exit 0
}
finally {
    Remove-Item Env:\PYTHONNOUSERSITE -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $venv) {
        Remove-Item -LiteralPath $venv -Recurse -Force -ErrorAction SilentlyContinue
    }
}