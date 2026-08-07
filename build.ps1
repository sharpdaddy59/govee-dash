# build.ps1 — arduino-cli build/upload/monitor wrapper for govee-dash.
#
# This project uses the mainstream esp32:esp32 core via arduino-cli. The
# CYD is a plain WROOM-32 with no OPI PSRAM, so the stock core is fine.
#
# Examples:
#   .\build.ps1                          # compile only
#   .\build.ps1 -Upload                  # compile + auto-detect + flash
#   .\build.ps1 -Upload -Monitor         # ... + serial @ 115200
#   .\build.ps1 -Upload -Port COM8       # specify port
#   .\build.ps1 -Strict                  # warnings=all
#
# Pre-reqs (run setup.ps1 once):
#   - arduino-cli on PATH
#   - esp32:esp32 core installed
#   - LovyanGFX, WiFiManager, ArduinoJson, NimBLE-Arduino installed

[CmdletBinding()]
param(
    [string]$Port = "",
    [switch]$Upload,
    [switch]$Monitor,
    [switch]$Strict
)

# Generic ESP32 dev board FQBN — works for the WROOM-32 module on the CYD.
$Fqbn = "esp32:esp32:esp32"

# PartitionScheme=min_spiffs gives a 1.9 MB app partition (vs the
# default scheme's 1.31 MB) by shrinking an unused SPIFFS partition.
# OTA support is preserved. NimBLE + LovyanGFX + WiFiManager +
# WebServer land close enough to the default slot that the headroom is
# worth keeping (hydro-dash hit 99% before this swap). Must be passed
# to both `compile` and `upload` so they agree on the build output path.
$PartitionScheme = "min_spiffs"
$BoardOpts       = "PartitionScheme=$PartitionScheme"

function Find-SketchDir {
    foreach ($c in @($PWD.Path, $PSScriptRoot) | Select-Object -Unique) {
        $leaf = Split-Path -Leaf $c
        if (Test-Path (Join-Path $c "$leaf.ino")) { return $c }
    }
    return $null
}
$SketchDir = Find-SketchDir
if (-not $SketchDir) {
    Write-Host "[build] No matching .ino found. arduino-cli requires <dir-leaf-name>/<dir-leaf-name>.ino."
    exit 1
}

$warn = if ($Strict) { "all" } else { "default" }
Write-Host "[build] Compiling (FQBN: $Fqbn, warnings=$warn)"

$compileArgs = @(
    "compile"
    "--fqbn"; $Fqbn
    "--board-options"; $BoardOpts
    "--warnings"; $warn
    "--export-binaries"
    $SketchDir
)
& arduino-cli @compileArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# arduino-cli's --export-binaries drops artifacts in a board-option-
# suffixed subdirectory of build/ whose exact name is version- and
# option-dependent. Locate the .bin rather than reconstructing the path.
$binPath = Get-ChildItem -Path (Join-Path $SketchDir "build") -Recurse `
               -Filter "$(Split-Path -Leaf $SketchDir).ino.bin" `
               -ErrorAction SilentlyContinue |
           Select-Object -First 1 -ExpandProperty FullName
if ($binPath) {
    Write-Host "[build] Firmware .bin: $binPath"
}

if (-not $Upload) {
    Write-Host "[build] Compile OK."
    return
}

if (-not $Port) {
    Write-Host "[build] Auto-detecting ESP32 port..."
    $raw = arduino-cli board list --format json | ConvertFrom-Json
    $boards = if ($raw.detected_ports) { $raw.detected_ports } else { $raw }

    # Score serial ports:
    #   3 = exact FQBN match
    #   1 = some other esp32:* board match
    #   0 = unknown / unrecognized chip (CH340, CP210x without driver db, etc.)
    # We collect ALL serial ports in the unknown bucket too, so we can
    # fall back to "single unknown serial port" — useful for CYD boards
    # whose CH340 USB IDs aren't recognised as ESP32 by arduino-cli.
    $candidates = @()
    $unknown = @()
    foreach ($b in $boards) {
        if ($b.port.protocol -ne "serial") { continue }
        $fqbns = @()
        if ($b.matching_boards) { $fqbns = @($b.matching_boards.fqbn) }
        $score = 0
        if ($fqbns -contains $Fqbn)                            { $score = 3 }
        elseif ($fqbns | Where-Object { $_ -like "esp32:*" })  { $score = 1 }
        if ($score -gt 0) {
            $candidates += [pscustomobject]@{ Port = $b.port.address; Score = $score; Fqbns = $fqbns }
        } else {
            $unknown += $b.port.address
        }
    }

    if ($candidates) {
        $best = $candidates | Sort-Object -Property Score -Descending | Select-Object -First 1
        $Port = $best.Port
        Write-Host "[build] Found ESP32 device on $Port"
    } elseif ($unknown.Count -eq 1) {
        # Fallback: exactly one unrecognised serial port — almost
        # certainly the CYD. Use it.
        $Port = $unknown[0]
        Write-Host "[build] No ESP32-tagged port detected; using the only serial port present: $Port"
        Write-Host "[build] (CH340 USB IDs aren't in arduino-cli's board database. If this isn't your CYD, pass -Port COMx explicitly.)"
    } elseif ($unknown.Count -gt 1) {
        Write-Host "[build] Multiple unrecognised serial ports detected: $($unknown -join ', ')"
        Write-Host "[build] Pass -Port COMx explicitly so the right one gets flashed."
        exit 1
    } else {
        Write-Host "[build] No serial port detected. Plug in the CYD or pass -Port COMx."
        exit 1
    }
}

Write-Host "[build] Uploading to $Port..."
# Pass the same board options so the upload step picks up the binary
# from the partition-scheme-suffixed build subdirectory.
arduino-cli upload -p $Port --fqbn $Fqbn --board-options $BoardOpts $SketchDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Monitor) {
    Write-Host "[build] Opening serial monitor on $Port at 115200. Ctrl+C to exit."
    arduino-cli monitor -p $Port -c baudrate=115200
}
