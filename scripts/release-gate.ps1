# Release gate, for Windows. The POSIX script is scripts/release-gate.sh; the
# checks and their wording are the same, and the two are meant to be edited
# together.
#
# The point is not to run the tests. The point is that "I ran the tests" is a
# recollection, and a release decision made from a recollection is how a red build
# ships.
#
#   pwsh -File scripts\release-gate.ps1
#   pwsh -File scripts\release-gate.ps1 -Tag v1.0.0

[CmdletBinding()]
param([string]$Tag = "")

$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo

$failures = New-Object System.Collections.Generic.List[string]
$notes = New-Object System.Collections.Generic.List[string]

function Step($text) { Write-Host ""; Write-Host "=== $text" }
function Ok($text) { Write-Host "  ok    $text" }
function Fail($text) {
    Write-Host "  FAIL  $text"
    $script:failures.Add($text)
}

Step "the tree is clean"
$dirty = git status --porcelain
if ([string]::IsNullOrWhiteSpace(($dirty -join ""))) {
    Ok "no uncommitted changes"
} else {
    $dirty | ForEach-Object { Write-Host "  $_" }
    Fail "uncommitted changes"
}

Step "version and tag"
$version = (git describe --tags --abbrev=0 2>$null)
if ([string]::IsNullOrWhiteSpace($version)) { $version = "none" }
Write-Host "  current tag: $version"
if ($Tag -ne "") {
    if ($version -eq $Tag) {
        Ok "HEAD is $Tag"
    } else {
        Fail "expected tag $Tag, found $version"
    }
} else {
    $notes.Add("no -Tag given, so this run checks the tree but not the tag")
}

Step "full verification (firmware, ESP32 build, backend, E2E)"
& powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-all.ps1
if ($LASTEXITCODE -eq 0) { Ok "verify-all.ps1" } else { Fail "verify-all.ps1" }

Step "SBOM and its pins"
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    Push-Location backend
    & python tools\sbom.py --out sbom\central.cdx.json --strict
    $sbomCode = $LASTEXITCODE
    Pop-Location
    if ($sbomCode -eq 0) { Ok "sbom.py --strict" } else { Fail "sbom.py --strict" }
} else {
    Fail "no python found for the SBOM step"
}

function Test-Docs($dir, $names) {
    $missing = @()
    foreach ($n in $names) {
        if (-not (Test-Path (Join-Path $dir $n))) { $missing += $n }
    }
    if ($missing.Count -eq 0) {
        Ok "every indexed document exists in $dir"
    } else {
        Fail ("missing from ${dir}: " + ($missing -join ", "))
    }
}

Step "documentation"
# Both lists are the full set of docs/ files. RUNBOOK.md and DOCUMENTATION_INDEX.md were
# missing from one side each, so the check passed while a document existed that no list
# mentioned - which is the drift this check exists to catch, applied to itself.
$en = @("RELEASE_READINESS.md", "ROADMAP.md", "BENCH_PLAN.md", "SECURITY.md",
    "OTA.md", "SYNC.md", "API.md", "BACKEND.md", "DATA_MODEL.md",
    "ARCHITECTURE.md", "CALIBRATION.md", "DEPLOYMENT.md", "HARDWARE.md",
    "TESTING.md", "DASHBOARD.md", "LEGAL.md", "PILOT_SPEC.md", "I18N.md",
    "DOMAIN_GLOSSARY.md", "DOCUMENTATION_INDEX.md", "RUNBOOK.md")
$es = @("RELEASE_READINESS.md", "ROADMAP.md", "BENCH_PLAN.md", "SECURITY.md",
    "API.md", "ARCHITECTURE.md", "BACKEND.md", "DATA_MODEL.md",
    "CALIBRATION.md", "DEPLOYMENT.md", "DASHBOARD.md", "HARDWARE.md",
    "I18N.md", "LEGAL.md", "OTA.md", "PILOT_SPEC.md", "SYNC.md", "TESTING.md",
    "DOMAIN_GLOSSARY.md", "DOCUMENTATION_INDEX.md", "RUNBOOK.md")
Test-Docs "docs/en" $en
Test-Docs "docs/es" $es

# The lists above are hand-maintained, so they drift. Compare them against the tree, in
# both languages, so a new document cannot ship without being listed and a renamed one
# cannot leave a stale entry behind.
foreach ($dir in @("docs/en", "docs/es")) {
    $onDisk = @(Get-ChildItem $dir -Filter *.md | Select-Object -ExpandProperty Name |
        Sort-Object)
    $listed = @($(if ($dir -eq "docs/en") { $en } else { $es }) | Sort-Object)
    $unlisted = @($onDisk | Where-Object { $_ -notin $listed })
    $phantom = @($listed | Where-Object { $_ -notin $onDisk })
    if ($unlisted.Count -eq 0 -and $phantom.Count -eq 0) {
        Ok "the $dir list matches the tree"
    } else {
        if ($unlisted.Count) { Fail ("not listed in the gate: " + ($unlisted -join ", ")) }
        if ($phantom.Count) { Fail ("listed but absent: " + ($phantom -join ", ")) }
    }
}

Step "the firmware signs with both algorithms"
# Checked in pieces rather than by grepping for one string, because the natural
# spelling varies: a header may hold the enum and the source may only spell it
# `cauce::FrameAlgorithm::kEd25519`. Each piece is something that must exist for
# Ed25519 frames to actually be signed and verified at the central.
$transportH = Get-Content firmware\lib\cauce_app\include\cauce\app\LoRaSyncTransport.h -Raw
$transportC = Get-Content firmware\lib\cauce_app\src\LoRaSyncTransport.cpp -Raw
$codecH = Get-Content firmware\lib\cauce_core\include\cauce\core\LoRaBatchCodec.h -Raw
$codecC = Get-Content firmware\lib\cauce_core\src\LoRaBatchCodec.cpp -Raw
$pieces = @{
    "transport exposes the algorithm"        = ($transportH -match "setFrameAlgorithm")
    "transport selects a trailer size"       = ($transportH -match "frameSignatureBytes")
    "codec declares the algorithm enum"      = ($codecH -match "kEd25519")
    "codec signs with ed25519"               = ($codecC -match "ed25519Sign")
    "codec signs with hmac"                  = ($codecC -match "hmacSha256")
    "transport refuses a bad key length"     = ($transportC -match "frameAlgorithmIsAsymmetric")
}
$allPresent = $true
foreach ($name in $pieces.Keys | Sort-Object) {
    if ($pieces[$name]) { Ok $name } else { Fail $name; $allPresent = $false }
}

Step "the group order constant is still the verified one"
# It was once typed from memory with a byte missing, which compiled cleanly and
# reduced to a plausible wrong scalar on every signature.
$points = Get-Content firmware\lib\cauce_core\src\Ed25519Points.cpp -Raw
if ($points -match 'edd3f55c') {
    Ok "kGroupOrder carries its verified hex"
} else {
    Fail "kGroupOrder lost its verified hex"
}

Step "no unregistered failing test is hiding in the tree"
$unregistered = Select-String -Path firmware\test\*.cpp -Pattern "NOT REGISTERED" |
    Select-Object -ExpandProperty Path -Unique
if ($unregistered) {
    Write-Host "  note  files mention NOT REGISTERED:"
    $unregistered | ForEach-Object { Write-Host "        $_" }
    $notes.Add("some tests are present but unregistered; check they are intentional")
} else {
    Ok "no unregistered tests"
}

Write-Host ""
Write-Host "=== summary"
foreach ($note in $notes) { Write-Host "  note  $note" }

Pop-Location

if ($failures.Count -eq 0) {
    Write-Host ""
    Write-Host "RELEASE GATE: PASS"
    Write-Host "tag: $version"
    Write-Host ""
    Write-Host "This says nothing is obviously broken. It does not certify the"
    Write-Host "hardware: docs/en/RELEASE_READINESS.md Phase 3 is still open."
    exit 0
}

Write-Host ""
Write-Host "RELEASE GATE: FAIL"
foreach ($failure in $failures) { Write-Host "  - $failure" }
exit 1
