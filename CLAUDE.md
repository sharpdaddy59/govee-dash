# CLAUDE.md — govee-dash project notes for Claude Code

Standalone dashboard firmware for the Sunton ESP32-2432S028R (CYD).
Passively scans Govee BLE hygrometer sensors (H5075/H5074/H5102/H5179)
and renders every sensor in range on an adaptive grid. Stateless —
never alerts, never decides; it displays and records.

**Heritage:** spun off from `../hydro-dash` (same hardware, same
conventions) with the cores3-hydro HTTP-polling half removed. When in
doubt about a convention, hydro-dash is the reference implementation.

## Build / flash / monitor

PowerShell, from the project root:

```powershell
.\build.ps1                  # compile only
.\build.ps1 -Upload          # compile + auto-detect port + flash
.\build.ps1 -Upload -Monitor # ... + serial @ 115200
.\build.ps1 -Strict          # warnings=all
```

One-time setup: `.\setup.ps1` installs arduino-cli, the mainstream
`esp32:esp32` core, and required libraries (LovyanGFX, WiFiManager,
ArduinoJson, **NimBLE-Arduino pinned @2.5.0**).

**Why mainstream esp32:esp32 not M5Stack's fork:** the CYD is a plain
WROOM-32 board with no OPI PSRAM; the mainstream core has better
long-term library compatibility.

**Why NimBLE-Arduino is pinned to 2.5.0:** the 2.x line is the one
compatible with arduino-esp32 3.x / ESP-IDF 5.x. The 1.4.x line
compiles but aborts at boot in `esp_bt_controller_init()`.

**The build's partition scheme is part of the build contract** —
`build.ps1` passes `--board-options PartitionScheme=min_spiffs` to both
`compile` and `upload`. When flashing manually with `arduino-cli
upload`, pass the same option or the output binary won't be found.

## Hardware map

| Subsystem | Pin / detail |
|-----------|--------------|
| ILI9341 TFT (HSPI) | MOSI 13, MISO 12, SCLK 14, CS 15, DC 2, BL 21 |
| LDR (auto-dim) | GPIO 34 |
| microSD (VSPI) | SCLK 18, MOSI 23, MISO 19, CS 5 |
| RGB LED (active LOW) | R 4, G 16, B 17 |

Reference: https://randomnerdtutorials.com/esp32-cheap-yellow-display-cyd-pinout-esp32-2432s028r/

## Architecture pointers

- **Boot order** (`govee-dash.ino`): `state_init` → `prefs_load` →
  `backlight_begin` → `ui_begin` → `wifi_setup_begin` (WiFiManager,
  blocks) → `MDNS.begin` → `ble_scanner_begin` → `sdlog_begin` →
  `http_server_begin`.
- **Concurrency:** the producer (`ble_scanner` task, core 0) writes
  atomics; consumers (`ui_grid`, `http_server`, `sdlog` on the loop
  task, core 1) read without locking. `g_devices_mutex` guards slot
  occupancy (insert/remove) only.
- **Tombstone device table** (`state.cpp`): slots never move. Insert
  populates fields then publishes `valid=true` LAST; remove clears
  `valid` FIRST then scrubs. This is what makes runtime removal
  (forget / ignore / auto-expiry) safe against the lock-free readers —
  hydro-dash's append-only table couldn't remove without a reboot.
  Do NOT "fix" this with compaction; a one-frame mixed read on slot
  reuse is accepted by design (the grid's layout-epoch check turns it
  into a clean full redraw).
- **Model dispatch** (`govee_decode.cpp`): pure decode functions keyed
  on (company ID, exact payload length). H5074/H5075 share CID 0xEC88
  and differ only by length. Wire formats + verification status in
  `docs/govee-ble.md`. Only H5075 is hardware-verified.
- **Grid redraw** (`ui_grid.cpp`): full clear only on a layout-epoch
  change (sensor set / count / config gen); otherwise per-tile snapshot
  diff with in-place glyph overwrite (`setTextColor(fg, bg)`) plus
  trailing band clears. No sprites — the CYD has no PSRAM.
  `g_ui_config_gen` (state.h) must be bumped by any handler whose
  change isn't visible in the per-tile value snapshot (alias, temp
  unit).
- **HTTP server:** synchronous `WebServer` polled from `loop()`.
- **NVS namespaces:** `gdash-ui` (schema, brightness, rot, unit, tz,
  expiry), `gdash-alias` (per-sensor names), `gdash-ignore` (MAC
  blocklist, RAM-cached with its own mutex — the scan task reads it
  per advert batch). Fresh namespaces — never reuse hydro-dash's
  `dash-*` ones.

## Critical gotchas (inherited from hydro-dash — all verified there)

1. **CYD-S028R LovyanGFX panel config is fiddly and non-obvious.** The
   working combination is in `ui.cpp::LGFX_CYD`: `panel_width=320,
   panel_height=240` (swapped from chip-native 240×320), `offset_y=80`,
   and rotation 4 (via `prefs`). Don't switch to `LGFX_AUTODETECT` —
   its runtime probe gives a white screen on this board. The schema
   value in `prefs.cpp::PREFS_SCHEMA` is what forces the right rotation
   on existing units after a config change.
2. **Colors must be `uint16_t`.** `setTextColor` with `uint32_t`
   dispatches to the RGB888 overload and reinterprets the bytes
   (TFT_GREEN comes out red). Also `rgb_order=true` stays set — the
   CYD's LCD is BGR-wired.
3. **GPIO 21 is shared.** Backlight and the P3 expansion header both
   use it. Wire something to P3 pin 4 and the panel goes dark.
4. **VSPI belongs to the microSD card.** SD access runs from `loop()`
   (`sdlog_loop`) — no task, no locking.
5. **GPIO 35 is input-only.**
6. **WiFiManager blocks** in `wifi_setup_begin()` until creds are
   submitted or `AP_TIMEOUT_S` (180 s) expires; on timeout we reboot.
7. **`web_assets.h` is generated.** Edit `web/index.html`, then run
   `pwsh tools/gen-web-assets.ps1` — the build embeds the committed
   header and never reads the HTML. An edit that isn't regenerated
   ships nothing.
8. **LDR polarity is inverted** (bright = LOW raw ADC) — see
   `backlight.cpp`; constants are named for the room condition.

## BLE bring-up / debugging

Set `BLE_DEBUG 1` at the top of `ble_scanner.cpp` to log every scan
window and decoded advert (raw hex + values). This is the required tool
for verifying the H5074/H5102/H5179 layouts against hardware — see
`docs/govee-ble.md` → "Verifying a model". The scan duty cycle
(config.h `BLE_SCAN_*`) is deliberately higher than hydro-dash's; if
WiFi/OTA gets flaky, the documented fallback is 160/48, 6 s, 4000 ms.

## Conventions for new work

- **New sensor model:** decoder + table row in `govee_decode.cpp`,
  enum + label; verify with BLE_DEBUG; document in `docs/govee-ble.md`.
- **New NVS-persisted state:** mirror existing namespaces in
  `prefs.cpp`. An *additive* key (new getter with a sensible default)
  needs no `PREFS_SCHEMA` bump — only bump when the meaning of an
  existing value changes.
- **Web settings UI:** edit `web/index.html`, regenerate
  `web_assets.h`. New settings reach the SPA through the `/config`
  endpoints in `http_server.cpp`.
- **User-facing changes:** bump `FW_VERSION` in `config.h`.

## Don'ts

- Don't add alerting — the dashboard is stateless by design.
- Don't hardcode WiFi credentials — WiFiManager onboarding is the one
  true path.
- Don't add auth assumptions to the HTTP API; LAN-trusted by design.
- Don't manually init SPI buses that LovyanGFX is already managing.
- Don't grow the dependency surface: ArduinoJson + LovyanGFX +
  WiFiManager + NimBLE-Arduino is the whole list.

## Where to look first

- `govee-dash.ino` — boot orchestration
- `config.h` — central tunables (pins, scan params, version)
- `govee_decode.cpp` — per-model decoders + dispatch table
- `ble_scanner.cpp` — scan task, pending buffer, staleness, expiry
- `state.h` / `state.cpp` — DeviceRecord, tombstone slots, atomics
- `ui.cpp` — LovyanGFX panel config (LOAD-BEARING, see gotcha #1)
- `ui_grid.cpp` — adaptive grid renderer
- `http_server.cpp` / `web/index.html` — management API + settings SPA
- `docs/govee-ble.md` — wire formats + verification procedure
