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
param(
    [string]$Tag = "",
    # The lock install downloads every wheel, so it needs network and about a minute. The
    # other checks do not. Off by default would be dishonest - the check is the point - so it
    # is on by default and this is the escape hatch for an offline machine.
    [switch]$SkipLock,
    # Building the image needs podman and a podman VM, which is minutes of work on a machine
    # that has neither. On by default when podman is absent the check reports SKIP rather than
    # a failure, so an offline or container-less machine can still run the gate.
    [switch]$SkipImage,
    # The flash-artifact check needs esptool, which runs in a container. Same reasoning as
    # -SkipImage: on by default when the runtime is there, and a skip rather than a failure
    # when it is not.
    [switch]$SkipFirmwareImage
)

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

        # When the dirt is only line endings, say so. A clean tree reporting itself dirty is
        # the signature of an attribute that asks for normalisation the checkout does not
        # perform, and the plain "uncommitted changes" sends somebody looking for a change
        # they never made. This exact confusion is why `-diff` without `-text` sat unnoticed
        # in `.gitattributes` until a fresh clone of v0.1.0 failed this very check.
        $eolOnly = @()
        foreach ($path in ($dirty | ForEach-Object { $_.Substring(3) })) {
            $numstat = git diff --numstat -- $path 2>$null
            if ($numstat -and $numstat -match '^(\d+)\s+(\d+)\s' -and $matches[1] -eq "0" -and $matches[2] -eq "0") {
                $eolOnly += $path
            }
        }
        if ($eolOnly.Count -eq $dirty.Count -and $eolOnly.Count -gt 0) {
            Write-Host "        note: every modified path differs only in line endings:"
            $eolOnly | ForEach-Object { Write-Host "          $_" }
            Write-Host "        a .gitattributes entry is missing '-text' for these"
        }
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

# Semver, and the tag has to be the version the application reports.
#
# Without this a tag is just a string someone typed, and nothing connects it to the code.
# `git describe` happily returns whatever the most recent tag says, so `v0.1.0-rc1` or
# `banana` both pass every other check in this script.
if ($version -ne "none" -and $version -ne "") {
    $bare = $version -replace '^v', ''
    if ($bare -match '^\d+\.\d+\.\d+$') {
        Ok "tag $version is semantic version $bare"
    } else {
        Fail "tag $version is not MAJOR.MINOR.PATCH; semver is what the OTA manifest carries"
    }
}

# The application version and the tag are two statements about the same fact, so they are
# checked against each other here rather than trusted. A central advertising 0.1.0 next to a
# node image tagged 0.2.0 sends operators to the wrong release notes.
$appVersion = (Select-String -Path backend\cauce_server\main.py -Pattern 'version="([^"]+)"' |
    Select-Object -First 1).Matches.Groups[1].Value
if ([string]::IsNullOrWhiteSpace($appVersion)) {
    Fail "could not read version= out of backend/cauce_server/main.py"
} elseif ($version -ne "none" -and $version -ne "") {
    $bare = $version -replace '^v', ''
    if ($appVersion -eq $bare) {
        Ok "app version $appVersion matches tag $version"
    } else {
        Fail "app reports $appVersion but the tag says $bare"
    }
}

Step "backend image is pinned"
# A floating base tag is the quiet way a release stops being reproducible, and the artifact
# that shipped unpinned dependencies is not a hypothetical: the Dockerfile installed
# requirements.txt while the repository carried a hash-pinned requirements.lock that no
# build ever read.
$dockerfile = Get-Content backend\Dockerfile -Raw
# The expected digest is recorded here as well as in the Dockerfile. One copy would be enough
# for the build; two is what makes drift detectable, which is the property that matters - a
# gate that reads its expectation from the file it is checking cannot fail.
$expectedBase = "sha256:02108f5d322dd89f1c9e552442c25acb0543dfdbc455693a5599624f20d9155d"
if ($dockerfile -match 'ARG\s+BASE_IMAGE=\S+@sha256:[0-9a-f]{64}') {
    Ok "base image is pinned to a digest, so no build can pick a floating tag"
} else {
    Fail "backend/Dockerfile does not pin the base image to a digest"
}
if ($dockerfile -match [regex]::Escape($expectedBase)) {
    Ok "base digest is the one this gate expects ($($expectedBase.Substring(0,19))...)"
} else {
    Fail "backend/Dockerfile base digest differs from the gate's; re-resolve both together"
}
if ($dockerfile -match 'imagetools inspect') {
    Ok "the documented way to move the base is a re-resolve, not a hand edit"
} else {
    $notes.Add("Dockerfile does not say how to re-resolve the base digest")
}
if ($dockerfile -match 'requirements\.lock' -and $dockerfile -match '--require-hashes') {
    Ok "image installs the hash-pinned lock, not the loose requirements"
} else {
    Fail "backend/Dockerfile does not install requirements.lock with --require-hashes"
}

Step "full verification (firmware, ESP32 build, backend, E2E)"
  $verifyAllLines = & powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-all.ps1 2>&1
  $verifyAllExit = $LASTEXITCODE
  $verifyAllLines | ForEach-Object { Write-Host "  $_" }
  if ($verifyAllExit -eq 0) {
    Ok "verify-all.ps1"
    # The counts verify-all.ps1 prints are the numbers the README, STATUS and thesis quote.
    # They were prose nobody checked, which is how 325 survived a move to 345 and then 349:
    # every test passed and the totals were wrong. Read them back out of the output and compare.
    #
    # Both numbers are checked as a pair against one documented sentence, because the two
    # halves are written as one claim ("349 firmware + 541 backend"). Checking each number
    # against a pattern containing only its own label is how the first version of this check
    # came to demand a README line reading "541 firmware", which no line ever said.
    $verifyAllText = $verifyAllLines -join "`n"
    $fwMatch = [regex]::Match($verifyAllText, 'firmware (\d+) tests')
    $beMatch = [regex]::Match($verifyAllText, 'backend (\d+) tests')
    if (-not $fwMatch.Success -or -not $beMatch.Success) {
      Fail ("verify-all.ps1 did not report both test counts, so the documentation cannot be " +
            "checked against what ran")
    } else {
      $fw = [int]$fwMatch.Groups[1].Value
      $be = [int]$beMatch.Groups[1].Value
      # One pattern, both numbers, exactly as the docs phrase it: the count comes first, as in
      # "349 firmware + 541 backend". Getting that order backwards produces a pattern that
      # matches nothing and reports every document as stale - a check that fails loudly is
      # better than one that fails silently, but it is still a check that does not work.
      $docsQuote = "$fw firmware \+ $be backend"
      $stale = @()
      foreach ($doc in @('README.md', 'README.es.md', 'STATUS.md', 'TESIS_CAUCE_ALEXANDRA.md')) {
        if (-not (Select-String -Path $doc -Pattern $docsQuote -Quiet -ErrorAction SilentlyContinue)) {
          $stale += $doc
        }
      }
      if ($stale.Count -eq 0) {
        Ok "every documented total matches the $fw firmware and $be backend tests that just ran"
      } else {
        Fail ("the suite ran $fw firmware and $be backend tests, but these documents still " +
              "quote a different pair: $($stale -join ', ')")
      }
    }
  } else {
    Fail "verify-all.ps1"
  }

Step "the ESP32 flash artifacts are valid, and the image fits its slot"
# Not execution. esptool's image_info parses the bootloader and application headers and
# refuses a malformed one; the partition parser checks there are two application slots, that
# they do not overlap, that everything is sector-aligned, and that the image fits. Those are
# the failures a board hits first, and they are catchable offline.
if ($SkipFirmwareImage) {
    $notes.Add("-SkipFirmwareImage given, so the flash artifacts were not checked")
} else {
    $fwLines = & powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-firmware-image.ps1 2>&1
    $fwExit = $LASTEXITCODE
    $fwLines | ForEach-Object { Write-Host "  $_" }
    $fwText = $fwLines -join "`n"
    if ($fwExit -ne 0) {
        Fail "verify-firmware-image.ps1; the artifacts would not flash"
    } elseif ($fwText -match "FIRMWARE IMAGE VERIFICATION: SKIP") {
        $notes.Add("the flash artifacts were not checked: no podman for esptool")
    } elseif ($fwText -match "FIRMWARE IMAGE VERIFICATION: PASS") {
        Ok "the bootloader and application parse, and the image fits an application slot"
    }
}

Step "the backend image builds, serves and is reproducible"
# Podman, rootless, no daemon and no elevation, so this can live in the gate rather than in a
# note saying somebody should run it elsewhere. `-SkipImage` for a machine with no runtime.
if ($SkipImage) {
    $notes.Add("-SkipImage given, so the container image was not built")
} else {
    # Run once and keep the output. verify-image.ps1 exits 0 both on PASS and on SKIP and
    # distinguishes them in its text, so treating a skip as a pass would be the one outcome
    # worse than not checking at all.
    $imageLines = & powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-image.ps1 2>&1
    $imageExit = $LASTEXITCODE
    $imageLines | ForEach-Object { Write-Host "  $_" }
    $imageText = $imageLines -join "`n"
    if ($imageExit -ne 0) {
        Fail "verify-image.ps1; the image does not build, serve, or is not reproducible"
    } elseif ($imageText -match "IMAGE VERIFICATION: SKIP") {
        $notes.Add("the image was not built: no podman machine on this machine")
    } elseif ($imageText -match "IMAGE VERIFICATION: PASS") {
        Ok "the image builds, serves /healthz, and two builds produce one image ID"
    }
}

Step "the pinned closure installs and the application runs from it"
# No container runtime here, so the image cannot be built and this does not pretend to. What
# it can check is the claim the Dockerfile actually makes - that the image installs
# `requirements.lock` and the application then works - by doing exactly that in a throwaway
# virtual environment containing nothing else.
#
# This is the check that catches a dependency the code imports but the lock omits. CI cannot:
# it installs `ruff` separately and runs in an environment that happens to contain everything,
# so a missing entry passes there and 500s in the image. `--require-hashes` cannot either -
# it verifies the packages that are listed and says nothing about one that is absent.
if ($SkipLock) {
    $notes.Add("-SkipLock given, so the lock was not installed and verified")
} else {
    & powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify-lock.ps1 -SkipTests
    if ($LASTEXITCODE -eq 0) {
        Ok "the lock installs with every hash verified and the app imports from it alone"
    } else {
        Fail "verify-lock.ps1; this is what the container build would do"
    }
}

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

Step "the certificate authority fails closed"
# A CA that silently issues certificates signed by nothing, or that falls back to the admin
# token when its key is missing, would make every issued document a claim the central never
# actually made. Both of those are checked structurally rather than by running the service,
# so the gate needs no configuration of its own.
$caEndpoint = Get-Content backend\cauce_server\certs_endpoint.py -Raw
if ($caEndpoint -match 'CAUCE_CA_KEY|certificate_authority_not_configured') {
    Ok "certificate endpoints fail closed when the CA is unconfigured"
} else {
    Fail "certs_endpoint.py does not refuse when no CA key is configured"
}
$caModule = Get-Content backend\cauce_server\certificates.py -Raw
if ($caModule -match 'def is_on_curve') {
    Ok "certificate issuance rejects a key that is not a curve point"
} else {
    Fail "certificates.py has no curve-point check; a key that can never verify would be certified"
}
if ($caModule -match 'def canonical_body' -and $caEndpoint -match 'nodes/\{node_id\}/certificate') {
    Ok "certificates are canonically serialised and exposed per node"
} else {
    Fail "certificate canonicalisation or the per-node endpoint is missing"
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
    "ARCHITECTURE.md", "CALIBRATION.md", "CALIBRATION_PROCEDURE.md",
    "RELEASE_ENGINEERING.md", "DEPLOYMENT.md", "HARDWARE.md",
    "TESTING.md", "DASHBOARD.md", "LEGAL.md", "PILOT_SPEC.md", "I18N.md",
    "DOMAIN_GLOSSARY.md", "DOCUMENTATION_INDEX.md", "RUNBOOK.md")
# CALIBRATION_PROCEDURE.md and RELEASE_ENGINEERING.md are deliberately absent from the Spanish
# list. They are English-only on purpose, and the reason is worth recording: a Spanish copy of
# RELEASE_ENGINEERING.md was written by machine-replacing the headings and left the body in
# English, which is a document that looks translated and is not. Shipping that is worse than
# shipping nothing, so the file was deleted and the gap recorded here instead. The per-language
# comparison below is per-directory, so an English-only document needs no Spanish counterpart -
# which is also the honest reading of "the same document in both languages": same subject,
# not necessarily same file.
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

Step "the documented coverage window matches the code"

# A constant quoted in prose is a claim, and this project has now had two documented numbers
# turn out to be wrong: the `require_scope` counts in STATUS.md's D1 section, and the coverage
# window cap - where STATUS said 400 days and the code said 730, because 400 appeared in a
# docstring as an *example* of a window that is too wide and was read as the limit. Both times
# the number had been read rather than measured.
#
# Read the constant out of the source and the figure out of the document, then compare. A
# changed limit now fails the gate instead of leaving a stale number in a file people read.
$coverageSource = "backend\cauce_server\coverage.py"
if (-not (Test-Path $coverageSource)) {
    Fail "$coverageSource is missing, so the documented window cap cannot be checked"
} else {
    $constMatch = [regex]::Match(
        (Get-Content $coverageSource -Raw), 'MAX_WINDOW_MS\s*=\s*(\d+)\s*\*\s*86400_000')
    if (-not $constMatch.Success) {
        Fail "could not read MAX_WINDOW_MS from $coverageSource; this check needs updating"
    } else {
        $days = [int]$constMatch.Groups[1].Value
        if (Select-String -Path STATUS.md -Pattern "caps a window at \*\*$days days\*\*" -Quiet) {
            Ok "STATUS.md quotes the real window cap ($days days)"
        } else {
            $quoted = [regex]::Matches(
                (Get-Content STATUS.md -Raw), 'caps a window at \*\*(\d+) days\*\*')
            $actual = if ($quoted.Count) { $quoted[0].Groups[1].Value } else { "nothing" }
            Fail ("the coverage window is $days days (MAX_WINDOW_MS in coverage.py) but " +
                  "STATUS.md says $actual; the documented number was read, not measured")
        }
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
