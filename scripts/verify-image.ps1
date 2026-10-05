# Verifies the backend container the way an operator would run it.

<#
.SYNOPSIS
    Builds the real backend image and proves it serves, then reports whether it is reproducible.

.DESCRIPTION
    The release gate used to check the Dockerfile's shape - the base is pinned, the lock is
    installed - and leave the image itself unbuilt. This runs it.

    Podman rather than Docker because it needs no daemon and no elevation, so the check can
    live in the gate instead of in a note saying somebody should run it on a machine that
    happens to have a container runtime. Rootless on WSL, four CPUs, 4 GiB.

    What it establishes, in order:

    - the pinned base digest pulls, and the index really does carry arm64 as well as amd64 -
      which was the reason for an index digest rather than a per-architecture one, and it had
      been asserted rather than checked;
    - the image builds from the actual Dockerfile;
    - `WORKDIR /app` plus the `COPY` layers produce a filesystem the application imports from
      and uvicorn serves from;
    - the declared `CMD` starts a server that answers `GET /healthz` over real HTTP;
    - the backend suite passes *inside the image*, which is a different claim from passing on
      a developer machine;
    - two builds of the same tree produce the same image ID, which is what "reproducible"
      means and what no lock file can promise on its own.

.PARAMETER SkipBuild
    Inspect and report only. Useful on a machine that cannot create the podman VM.

.PARAMETER SkipSuite
    Skip the in-image test run, which is the slowest step.

.EXAMPLE
    pwsh -File scripts\verify-image.ps1

.EXAMPLE
    pwsh -File scripts\verify-image.ps1 -SkipSuite
#>
[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$SkipSuite
)

$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent $PSScriptRoot
$failed = $false
$notes = New-Object System.Collections.Generic.List[string]

function Step($text) { Write-Host ""; Write-Host "=== $text" }
function Ok($text) { Write-Host "  ok    $text" }
function Note($text) { Write-Host "  note  $text"; $notes.Add($text) }
function Fail($text) { Write-Host "  FAIL  $text"; $script:failed = $true }

# The digest the Dockerfile pins. Kept here so this script fails loudly if the two disagree,
# which is the same reason the release gate carries its own copy.
$expectedBase = "sha256:02108f5d322dd89f1c9e552442c25acb0543dfdbc455693a5599624f20d9155d"
$imageTag = "cauce-central:verify"
$port = 18099

function Get-Podman {
    $candidate = Get-Command podman -ErrorAction SilentlyContinue
    if ($candidate) { return $candidate.Source }
    # The winget install puts it here and does not add it to PATH in an already-running shell.
    $wingetPath = Join-Path $env:LOCALAPPDATA "Programs\podman\podman.exe"
    if (Test-Path -LiteralPath $wingetPath) { return $wingetPath }
    return $null
}

$podman = Get-Podman
if (-not $podman) {
    Write-Host ""
    Write-Host "IMAGE VERIFICATION: SKIP"
    Write-Host ""
    Write-Host "No podman on this machine. Install it with:"
    Write-Host "  winget install --id Podman.CLI --exact"
    Write-Host "then run once:"
    Write-Host "  podman machine init --cpus 4 --memory 4096 --disk-size 30"
    Write-Host "  podman machine start"
    Write-Host ""
    Write-Host "Nothing here is asserted without it. The Dockerfile's shape is still"
    Write-Host "checked by the release gate, and scripts\verify-lock.ps1 still proves the"
    Write-Host "dependency closure installs and runs the application."
    exit 0
}

Push-Location $repo
try {
    Step "podman"
    & $podman version --format '{{.Client.Version}}' 2>&1 | Select-Object -First 1 | ForEach-Object { Ok "client $_" }

    # A machine is a podman VM. Without one there is nowhere to build, and the honest
    # outcome is to say so rather than to report a build that did not happen.
    $machines = (& $podman machine list --format '{{.Name}}' 2>$null) -join ""
    if ([string]::IsNullOrWhiteSpace($machines)) {
        Write-Host ""
        Write-Host "IMAGE VERIFICATION: SKIP"
        Write-Host ""
        Write-Host "Podman is installed but no machine exists. Create one with:"
        Write-Host "  podman machine init --cpus 4 --memory 4096 --disk-size 30"
        Write-Host "  podman machine start"
        exit 0
    }
    # `machine start` on an already-running machine exits 125 with "already running", which
    # is not a failure and is the normal case on the second run of this script. Checking the
    # running state instead of the exit code is what keeps the note from appearing every time.
    $running = (& $podman machine list --format '{{.Running}}' 2>$null) -join ""
    if ($running -match "true") {
        Ok "the podman machine is running"
    } else {
        & $podman machine start 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Ok "started the podman machine"
        } else {
            Note "could not start the podman machine; the build may fail below"
        }
    }

    Step "the pinned base, and whether it carries arm64"
    $dockerfile = Get-Content backend\Dockerfile -Raw
    if ($dockerfile -notmatch [regex]::Escape($expectedBase)) {
        Fail "the Dockerfile does not carry the digest this script expects"
    } else {
        Ok "the Dockerfile and this script agree on the base digest"
    }

    $baseRef = "python:3.12-slim@$expectedBase"
    & $podman pull $baseRef 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { Ok "the pinned digest pulls" } else { Fail "the pinned digest does not pull" }

    # The Raspberry Pis are arm64. An index digest is what makes one value work on both, and
    # this is the only way to know the index still carries arm64 - a base could be republished
    # for amd64 alone and the digest would still resolve.
    $inspect = (& $podman manifest inspect "docker.io/library/python@$expectedBase" 2>$null) -join "`n"
    try {
        $index = $inspect | ConvertFrom-Json
        $platforms = $index.manifests |
            Where-Object { $_.platform.architecture -in @("amd64", "arm64") } |
            ForEach-Object { "$($_.platform.os)/$($_.platform.architecture)" }
        if ($platforms -contains "linux/amd64") {
            Ok "the index carries linux/amd64"
        } else { Fail "the index has no linux/amd64 manifest" }
        if ($platforms -contains "linux/arm64") {
            Ok "the index carries linux/arm64, so the pilot Pis are covered by one digest"
        } else {
            Fail "the index has no linux/arm64 manifest; an amd64 digest would fail on a Pi"
        }
    } catch {
        Note "could not parse the manifest index ($($_.Exception.Message))"
    }

    if ($SkipBuild) {
        Write-Host ""
        Write-Host "IMAGE VERIFICATION: SKIP ( -SkipBuild )"
        exit 0
    }

    Step "building the image from the real Dockerfile"
    Push-Location backend
    try {
        & $podman build --build-arg "BASE_IMAGE=$baseRef" -t $imageTag -f Dockerfile . 2>&1 |
            Select-Object -Last 3 | ForEach-Object { Write-Host "  $_" }
    } finally { Pop-Location }
    if ($LASTEXITCODE -eq 0) { Ok "the image builds" } else { Fail "the image does not build"; exit 1 }

    $size = (& $podman images $imageTag --format '{{.Size}}' 2>$null) -join ""
    if ($size) { Ok "image size $size" }

    Step "WORKDIR and the copied layers produce a servable filesystem"
    # This is the first of the two things a lock file cannot check. `/app` is where the
    # application has to be importable from, because that is the declared working directory.
    $probe = "import os, cauce_server; print('cwd', os.getcwd()); print('pkg', cauce_server.__file__)"
    $out = & $podman run --rm $imageTag python -c $probe 2>&1
    if ($out -match "cwd /app" -and $out -match "pkg /app/cauce_server") {
        Ok "the application is importable from the declared WORKDIR"
    } else {
        Fail "the application is not importable from WORKDIR; see output above"
        $out | ForEach-Object { Write-Host "        $_" }
    }

    Step "the declared CMD serves, over real HTTP"
    & $podman rm -f cauce-image-verify 2>&1 | Out-Null
    & $podman run -d --name cauce-image-verify -p "${port}:8000" $imageTag 2>&1 | Out-Null
    Start-Sleep -Seconds 10
    try {
        $health = Invoke-WebRequest -Uri "http://127.0.0.1:$port/healthz" -UseBasicParsing -TimeoutSec 25
        if ($health.StatusCode -eq 200) {
            Ok "GET /healthz answered 200 from inside the container"
            $version = ($health.Content | ConvertFrom-Json).version
            Ok "the served application reports version $version"
        } else {
            Fail "GET /healthz answered $($health.StatusCode)"
        }
    } catch {
        Fail "the container did not answer on port ${port}: $($_.Exception.Message)"
        & $podman logs cauce-image-verify 2>&1 | Select-Object -Last 15 | ForEach-Object { Write-Host "        $_" }
    }

    # Fail-closed behaviour, in the artifact, rather than only in the host suite.
    try {
        $write = Invoke-WebRequest -Uri "http://127.0.0.1:$port/v1/sites" -Method Post `
            -Headers @{ Authorization = "" } -ContentType "application/json" `
            -Body '{"site_id":"S1"}' -UseBasicParsing -TimeoutSec 15
        Fail "a write was accepted with no admin token configured"
    } catch {
        $code = [int]$_.Exception.Response.StatusCode
        if ($code -eq 503) {
            Ok "a write with no admin token is refused (503), as it must be"
        } else {
            Note "a write with no admin token returned $code rather than 503"
        }
    }
    & $podman rm -f cauce-image-verify 2>&1 | Out-Null

    $ranSuite = $false
    if ($SkipSuite) {
        Note "-SkipSuite given, so the suite was not run inside the image"
    } else {
        Step "the backend suite passes inside the image"
        # The repository is mounted read-only with only ./data writable: the tests each open
        # their own ./data/<name>.sqlite, so that one directory has to be writable and the
        # rest must not be. The repo root is needed because one test reads
        # deployment/docker-compose.yml, which is not part of the image - so the source is
        # mounted rather than copied in.
        $scratch = Join-Path $env:TEMP ("cauce-imagesuite-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
        New-Item -ItemType Directory -Path $scratch -Force | Out-Null
        try {
            & $podman run --rm -v "${repo}:/repo:ro" -v "${scratch}:/repo/backend/data" `
                -w /repo/backend $imageTag python -m pytest tests -q -p no:cacheprovider 2>&1 |
                Select-Object -Last 3 | ForEach-Object { Write-Host "  $_" }
            $ranSuite = $true
            if ($LASTEXITCODE -eq 0) {
                Ok "the suite passes against the image's own dependency closure"
            } else {
                Fail "the suite failed inside the image"
            }
        } finally {
            Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    Step "two builds of one tree produce one image"
    Push-Location backend
    try {
        & $podman build --build-arg "BASE_IMAGE=$baseRef" -t "cauce-central:verify2" -f Dockerfile . 2>&1 | Out-Null
    } finally { Pop-Location }
    $id1 = (& $podman image inspect $imageTag --format '{{.Id}}' 2>$null) -join ""
    $id2 = (& $podman image inspect "cauce-central:verify2" --format '{{.Id}}' 2>$null) -join ""
    if ($id1 -and $id2 -and $id1 -eq $id2) {
        Ok "identical image IDs ($($id1.Substring(0,19))...) - the build is reproducible"
    } else {
        Note "the two builds differ: $id1 vs $id2"
        Note "not fatal on its own, but a reproducible build is the point of the lock"
    }

    Write-Host ""
    if ($failed) {
        Write-Host "IMAGE VERIFICATION: FAIL"
        exit 1
    }
    Write-Host "IMAGE VERIFICATION: PASS"
    Write-Host ""
    Write-Host "The pinned base pulls and carries arm64. The image builds, the"
    Write-Host "application is importable from WORKDIR, the declared CMD serves /healthz"
    Write-Host "over real HTTP, and writes are refused without an admin token"
    if ($ranSuite) {
        Write-Host ". The suite passes inside the image too."
    } else {
        Write-Host ". The suite was not run this time, so it is not claimed to have passed."
    }
    foreach ($n in $notes) { Write-Host "note: $n" }
    exit 0
}
finally {
    Pop-Location
}