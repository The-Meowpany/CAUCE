# Validates the ESP32 flash artifacts with esptool, without a board.

<#
.SYNOPSIS
    Runs esptool's image checks over the real bootloader, partition table and firmware.

.DESCRIPTION
    Nothing here executes the firmware. QEMU has no ESP32 machine - it models dc232b and
    lx6 development boards, and `grep -c esp32` on its machine list returns zero - so there is
    no emulator that would let this repository's firmware run without silicon. That is
    recorded in STATUS.md rather than worked around, because a QEMU that modelled the wrong
    Xtensa core would produce results that look like hardware evidence and are not.

    What esptool *can* do is check the artifacts a board would be flashed with, and these are
    real checks rather than a formality:

    - the image header, flash mode, flash size and frequency, which are the fields that decide
      whether a board boots at all;
    - the Segel section, the ESP32 image magic, the entry point and the SPI flash header, all
      of which esptool refuses if they are malformed;
    - the SHA-256 of the image, recorded so two builds can be compared;
    - that the partition table parses and that the application partition is large enough for
      the firmware that is meant to go in it - a real failure mode, and the reason the two-slot
      OTA table needs a 4 MB part.

    None of that is a substitute for a board. It is the difference between "the compiler
    produced a file" and "the file is a valid ESP32 image", which is worth having and is
    usually not checked.

.PARAMETER SkipChecksum
    Do not append the image hashes to firmware/artifacts.sha256.

.EXAMPLE
    pwsh -File scripts\verify-firmware-image.ps1
#>
[CmdletBinding()]
param(
    [string]$Environment = "esp32dev",
    [switch]$SkipChecksum
)

$ErrorActionPreference = "Continue"
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo "firmware\.pio\build\$Environment"
$failed = $false

function Step($text) { Write-Host ""; Write-Host "=== $text" }
function Ok($text) { Write-Host "  ok    $text" }
function Fail($text) { Write-Host "  FAIL  $text"; $script:failed = $true }

function Get-Podman {
    $candidate = Get-Command podman -ErrorAction SilentlyContinue
    if ($candidate) { return $candidate.Source }
    $wingetPath = Join-Path $env:LOCALAPPDATA "Programs\podman\podman.exe"
    if (Test-Path -LiteralPath $wingetPath) { return $wingetPath }
    return $null
}

foreach ($artifact in @("bootloader.bin", "partitions.bin", "firmware.bin")) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $artifact))) {
        Write-Host "  FAIL  $artifact is not built. Run: cd firmware; pio run -e $Environment"
        exit 1
    }
}

$podman = Get-Podman
if (-not $podman) {
    Write-Host ""
    Write-Host "FIRMWARE IMAGE VERIFICATION: SKIP"
    Write-Host ""
    Write-Host "Needs podman for esptool. Install with:"
    Write-Host "  winget install --id Podman.CLI --exact"
    Write-Host "  podman machine init --cpus 4 --memory 4096 --disk-size 30"
    Write-Host "  podman machine start"
    Write-Host ""
    Write-Host "Nothing is asserted without it. Note that no ESP32 emulator exists in QEMU,"
    Write-Host "so this can never be a substitute for a board: see STATUS.md."
    exit 0
}

Push-Location $repo
try {
    Step "esptool"
    $version = (& $podman run --rm docker.io/library/debian:stable sh -c `
        "apt-get update -qq 2>/dev/null; apt-get install -y esptool >/dev/null 2>&1; esptool version 2>&1 | head -1" 2>&1) -join " "
    if ($version -match "esptool") {
        Ok "$($version.Trim()) in a container, so the check needs nothing on the host"
    } else {
        Fail "could not run esptool in a container: $version"
        exit 1
    }

    Step "the bootloader image"
    # `image-info` is esptool's structural check: it parses the header, the Segel section and
    # the SPI flash parameters, and refuses a malformed image rather than reporting on it.
    $info = & $podman run --rm -v "${build}:/img:ro" docker.io/library/debian:stable sh -c `
        "apt-get update -qq 2>/dev/null; apt-get install -y esptool >/dev/null 2>&1; esptool --chip esp32 image_info /img/bootloader.bin 2>&1"
    $info | ForEach-Object { Write-Host "      $($_.ToString().Trim())" }
    $infoText = $info -join "`n"
    if ($infoText -match "Validation Hash.*valid" -and $infoText -match "Checksum:.*valid") {
        Ok "the bootloader parses as a valid ESP32 image, checksum and validation hash both good"
    } else {
        Fail "esptool did not confirm the bootloader's checksum and validation hash"
    }

    Step "the partition table"
    # The parser is a file, not an inline program: a struct unpack inside a PowerShell string
    # inside a container `sh -c` is three quoting layers, and the inline version was a
    # PowerShell parse error rather than a script that ran and failed.
    $parts = & $podman run --rm -v "${build}:/img:ro" -v "${repo}\scripts:/scripts:ro" `
        docker.io/library/python:3.12-slim python /scripts/partition_report.py /img/partitions.bin 2>&1
    $parts | ForEach-Object { Write-Host "      $($_.ToString().Trim())" }
    if ($LASTEXITCODE -eq 0) {
        # The parser is the authority on its own findings: it exits non-zero on a missing
        # second slot, on overlapping partitions, and on a bad MD5. Duplicating those rules
        # in PowerShell here would be a second implementation that could disagree with the
        # first, which is how a check ends up passing.
        Ok "the partition table parses: two application slots, no overlap, sizes as declared"
    } else {
        Fail "the partition table did not pass its own checks"
    }
    # The declared table is the claim; the binary is the artifact. Cross-checking them means a
    # regenerated table that silently lost a slot fails here rather than on a board that
    # cannot roll back.
    # Both of these are joined into single strings before any `-match`.
    #
    # `Get-Content` returns an ARRAY, and PowerShell's `-match` against an array returns an
    # ARRAY of booleans, not a boolean - so the condition was true, every partition was
    # reported absent, and `$matches` was left holding a stale group from an earlier match,
    # which then blew up `[Convert]::ToInt64` with an out-of-range index. All five partitions
    # are present in the artifact above; the check was reading the wrong type.
    $partsText = ($parts | ForEach-Object { $_.ToString() }) -join "`n"
    $declaredText = (Get-Content (Join-Path $repo "firmware\partitions.csv")) -join "`n"
    foreach ($expected in @("app0", "app1", "otadata", "nvs", "spiffs")) {
        if ($declaredText -match "(?m)^\s*$expected\s*,") {
            if ($partsText -match "(?m)^\s*$expected\s") { Ok "$expected is declared and present" }
            else { Fail "$expected is declared in partitions.csv but absent from the image" }
        } else {
            Fail "$expected is absent from partitions.csv"
        }
    }
    # The slot has to be able to hold the firmware, which is over 1 MB. Read the slot size from
    # the declared CSV rather than the binary: the CSV is what a reviewer reads, and its unit
    # is 4 KiB sectors, which is the thing that is easy to misread as bytes.
    if ($declaredText -match "(?m)^\s*app0\s*,\s*app\s*,\s*ota_0\s*,\s*0x[0-9a-fA-F]+\s*,\s*0x([0-9a-fA-F]+)") {
        $slotBytes = [Convert]::ToInt64($matches[1], 16)
        $slotKb = [int]($slotBytes / 1024)
        $fwKb = [math]::Round((Get-Item (Join-Path $build "firmware.bin")).Length / 1KB)
        if ($slotKb -gt $fwKb) {
            Ok "the declared app slot is ${slotKb}KB for a ${fwKb}KB image"
        } else {
            Fail "the declared app slot is ${slotKb}KB and the image is ${fwKb}KB; it will not fit"
        }
    } else {
        Fail "could not read the app0 slot size out of partitions.csv"
    }

    Step "the application image"
    $appInfo = & $podman run --rm -v "${build}:/img:ro" docker.io/library/debian:stable sh -c `
        "apt-get update -qq 2>/dev/null; apt-get install -y esptool >/dev/null 2>&1; esptool --chip esp32 image_info /img/firmware.bin 2>&1"
    $appText = $appInfo -join "`n"
    $appInfo | Select-Object -Last 10 | ForEach-Object { Write-Host "      $($_.ToString().Trim())" }
    if ($appText -match "Validation Hash.*valid" -and $appText -match "Checksum:.*valid") {
        Ok "the application image parses, checksum and validation hash both good"
    } else {
        Fail "esptool did not confirm the application image's checksum and validation hash"
    }
    if ($appText -match "Entry point:\s*(\S+)") {
        Ok "entry point $($matches[1]) is recorded, which is what a bootloader jumps to"
    } else {
        Fail "esptool did not report an entry point for the application image"
    }

    Step "the image size against the OTA safety gate"
    # The OTA path compares the manifest's `total_size` against what it actually flashes and
    # refuses a mismatch, so an image that grew past its slot is caught by the *node*, at
    # update time, on a board in the field. Catching it here is the whole point of running
    # the check offline.
    $fwBytes = (Get-Item (Join-Path $build "firmware.bin")).Length
    $fwKb = [math]::Round($fwBytes / 1KB)
    Ok "firmware.bin is ${fwKb}KB"
    if ($declaredText -match "(?m)^\s*app0\s*,\s*app\s*,\s*ota_0\s*,\s*0x[0-9a-fA-F]+\s*,\s*0x([0-9a-fA-F]+)") {
        $slotBytes = [Convert]::ToInt64($matches[1], 16)
        if ($fwBytes -lt $slotBytes) {
            Ok "the image fills $([math]::Round(100.0 * $fwBytes / $slotBytes, 1))% of the $([int]($slotBytes/1024))KB slot"
        } else {
            Fail "the image is ${fwKb}KB and the slot is $([int]($slotBytes/1024))KB; it cannot be flashed"
        }
    }

    if (-not $SkipChecksum) {
        Step "recording the artifact hashes"
        $lines = @("# SHA-256 of the ESP32 flash artifacts, for comparing two builds.")
        $lines += "# Regenerate with: scripts\verify-firmware-image.ps1"
        $lines += "# Not a signature: it says which bytes were produced, not who produced them."
        $lines += "# Environment: $Environment"
        foreach ($artifact in @("bootloader.bin", "partitions.bin", "firmware.bin")) {
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $build $artifact)).Hash.ToLower()
            $lines += "$hash  $artifact"
        }
        $lines += ""
        $outFile = Join-Path $repo "firmware\artifacts.sha256"
        [System.IO.File]::WriteAllLines($outFile, $lines, (New-Object System.Text.UTF8Encoding($false)))
        Ok "wrote firmware\artifacts.sha256"
    }

    Write-Host ""
    if ($failed) {
        Write-Host "FIRMWARE IMAGE VERIFICATION: FAIL"
        Write-Host ""
        Write-Host "These are the checks a board would hit first. They are NOT execution:"
        Write-Host "nothing here ran the firmware, and no ESP32 emulator exists in QEMU."
        exit 1
    }
    Write-Host "FIRMWARE IMAGE VERIFICATION: PASS"
    Write-Host ""
    Write-Host "The bootloader, partition table and application all parse as valid ESP32"
    Write-Host "artifacts, the table has two application slots, and the slot is larger than"
    Write-Host "the image."
    Write-Host ""
    Write-Host "What this is not: none of it executed the firmware. QEMU models dc232b and"
    Write-Host "lx6 boards and has no esp32 machine, so there is no way to run this on"
    Write-Host "this machine. Deep sleep, the watchdog, OTA over the air, the SX1276 radio"
    Write-Host "and every I2C peripheral still require silicon."
    exit 0
}
finally {
    Pop-Location
}