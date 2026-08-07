# govee-dash

A standalone wall/desk dashboard for **Govee Bluetooth thermometer /
hygrometer sensors**, running on the ~$12 Sunton ESP32-2432S028R
("Cheap Yellow Display" / CYD).

It passively listens to the BLE advertisements Govee sensors already
broadcast — no pairing, no Govee hub, no phone app, no cloud — and shows
every sensor in range on an adaptive grid:

- **1 sensor** — full-screen readout
- **2 sensors** — two full-width rows
- **3–4 sensors** — 2×2 tiles
- **5–6 sensors** — 3×2 tiles
- **7+** — first six tiled, a `+N` badge for the rest

Each tile shows the sensor's name (or your alias), temperature (°C/°F),
humidity, battery, and signal strength. Fresh readings are green; a
sensor unheard for 90 s grays out; low battery goes yellow.

## Supported sensors

| Model | Status |
|-------|--------|
| Govee H5075 | verified against hardware |
| Govee H5074 | decoder present — needs first-device verification |
| Govee H5102 | decoder present — needs first-device verification |
| Govee H5179 | decoder present — needs first-device verification |

Wire formats are documented in [docs/govee-ble.md](docs/govee-ble.md),
including how to verify a new model with the built-in BLE debug logging
and how to add more models.

## Features

- **Web settings console** at `http://govee-dash-xxxx.local/` —
  rename sensors, forget/ignore sensors, °C/°F, backlight, auto-forget
  timeout, timezone.
- **REST API** — `GET /devices`, `/config`, `/status` JSON for your own
  automations (pull-only; the dashboard itself never alerts or decides).
- **microSD CSV logging** — insert a card and readings append to a
  monthly `/govee-YYYY-MM.csv`, downloadable from the browser.
- **OTA updates** — upload a new firmware `.bin` from the settings page.
- **WiFiManager onboarding** — first boot opens a `govee-dash-xxxx-setup`
  access point; join it and enter your WiFi credentials. No hardcoded
  secrets.
- **LDR auto-dim** — the CYD's light sensor dims the backlight at night.

## Build

One-time setup (installs arduino-cli, the esp32 core, and libraries):

```powershell
.\setup.ps1
```

Then:

```powershell
.\build.ps1                  # compile only
.\build.ps1 -Upload          # compile + auto-detect port + flash
.\build.ps1 -Upload -Monitor # ... + serial monitor @ 115200
```

If you edit `web/index.html`, regenerate the embedded copy first:

```powershell
pwsh tools/gen-web-assets.ps1
```

## Heritage

Spun off from [hydro-dash](../hydro-dash), a hydroponics dashboard for
the same hardware — govee-dash keeps the Govee BLE side, the display
stack, and the web console, and drops the hydroponics-specific HTTP
polling. Unlike hydro-dash, the sensor table supports removal at
runtime (forget / ignore / auto-expiry).
