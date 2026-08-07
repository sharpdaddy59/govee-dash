# setup.ps1 — one-time arduino-cli configuration for govee-dash.
#
# Idempotent — safe to re-run. Installs:
#   - arduino-cli itself (via winget) if missing
#   - The mainstream esp32:esp32 core (no M5Stack fork needed for CYD)
#   - Required libraries (LovyanGFX, WiFiManager, ArduinoJson, NimBLE-Arduino)
#
# Run once after cloning. After this completes, .\build.ps1 is the daily
# entry point.

$ErrorActionPreference = 'Stop'

# The Arduino sketchbook — the parent directory this sketch lives in.
$Sketchbook = Split-Path $PSScriptRoot -Parent

function Have-Cmd($name) {
    $null -ne (Get-Command $name -ErrorAction SilentlyContinue)
}

if (-not (Have-Cmd 'arduino-cli')) {
    Write-Host '[setup] arduino-cli not found; installing via winget...'
    winget install --id ArduinoSA.CLI -e --accept-source-agreements --accept-package-agreements
    if (-not (Have-Cmd 'arduino-cli')) {
        Write-Host '[setup] winget installed arduino-cli, but it is not on PATH for this shell.'
        Write-Host '[setup] Open a NEW PowerShell window and re-run setup.ps1.'
        exit 1
    }
} else {
    Write-Host ('[setup] arduino-cli found: ' + ((arduino-cli version) -join ' '))
}

Write-Host '[setup] Initializing arduino-cli config (no-op if it already exists)...'
arduino-cli config init 2>$null | Out-Null

Write-Host "[setup] Pointing sketchbook at $Sketchbook"
arduino-cli config set directories.user "$Sketchbook"

Write-Host '[setup] Updating package index...'
arduino-cli core update-index

Write-Host '[setup] Installing esp32:esp32 core (mainstream Espressif arduino-esp32)...'
arduino-cli core install esp32:esp32

$libs = @(
    'LovyanGFX'
    'WiFiManager'
    'ArduinoJson'
    # NimBLE-Arduino — passive BLE advertisement scan (Govee sensors).
    # Pinned to the 2.x line: it is the version compatible with the
    # arduino-esp32 3.x core (ESP-IDF 5.x) installed above. The 1.4.x line
    # targets IDF 4.x — it compiles against a 3.x core but aborts at boot
    # in esp_bt_controller_init(). ble_scanner.cpp uses the 2.x scan API.
    'NimBLE-Arduino@2.5.0'
)
foreach ($lib in $libs) {
    Write-Host "[setup] Installing library: $lib"
    arduino-cli lib install "$lib"
}

Write-Host ''
Write-Host '[setup] Verifying ESP32 generic board is recognized...'
arduino-cli board listall esp32 | Select-String -Pattern 'esp32:esp32:esp32 ' -CaseSensitive:$false

Write-Host ''
Write-Host '[setup] Done. Try a build with:    .\build.ps1'
Write-Host '[setup] Plug in the CYD then:      .\build.ps1 -Upload -Monitor'
