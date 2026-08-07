// config.h — central tunables for govee-dash.
//
// Pinout matches the Sunton ESP32-2432S028R (CYD) per
// https://randomnerdtutorials.com/esp32-cheap-yellow-display-cyd-pinout-esp32-2432s028r/
//
// If you have a different revision (S028C capacitive, or one of the
// silently-different clone variants), the pin map below is the place to fix.

#pragma once

#define FW_VERSION       "0.1.0"

// ---------------------------------------------------------------------------
// Display (ILI9341, HSPI bus, 240x320 portrait native -> rotated to 320x240)
// ---------------------------------------------------------------------------
#define TFT_MOSI         13
#define TFT_MISO         12
#define TFT_SCLK         14
#define TFT_CS           15
#define TFT_DC           2
#define TFT_RST          -1   // tied to ESP32 reset; software reset only
#define TFT_BL           21   // backlight, active HIGH, PWM-capable
#define TFT_BL_PWM_CH    0
#define TFT_BL_PWM_FREQ  5000
#define TFT_BL_PWM_BITS  8

#define TFT_W            320
#define TFT_H            240

// ---------------------------------------------------------------------------
// On-board sensors / indicators
// ---------------------------------------------------------------------------
#define LDR_PIN          34   // ADC1, input-only
#define SPEAKER_PIN      26   // not used by default; reserved
#define LED_R_PIN        4    // active LOW
#define LED_G_PIN        16
#define LED_B_PIN        17

// ---------------------------------------------------------------------------
// App behavior
// ---------------------------------------------------------------------------
#define MAX_DEVICES      8    // device-table slots (tombstoned, reusable)
#define GRID_MAX_TILES   6    // tiles shown on screen; extras get a "+N" badge

// ---------------------------------------------------------------------------
// BLE sensor scanning — Govee hygrometers, passive advertisement scan.
//
// The CYD's ESP32-WROOM-32 shares one 2.4 GHz radio between WiFi and BLE.
// Unlike hydro-dash (which this project derives from), there is no
// continuous HTTP polling load to protect, so the scan duty cycle is
// raised: 60/100 window/interval during a burst (~60%), 8 s window,
// 2 s gap — roughly 48% overall listening time. Do NOT go gapless:
// the coexistence arbiter still needs clear air for WiFi beacons, the
// web SPA's 5 s poll, NTP, and OTA uploads; starvation shows up as
// WiFi disconnects. If OTA uploads get flaky, first restore the
// hydro-dash values (160/48, 6 s, 4000 ms gap).
//
// Per-model company IDs and payload layouts live in govee_decode.cpp.
// ---------------------------------------------------------------------------
#define BLE_SCAN_DURATION_S      8      // length of one scan window
#define BLE_SCAN_GAP_MS          2000   // idle gap between windows (WiFi breather)
#define BLE_SCAN_INTERVAL_MS     100    // controller scan interval
#define BLE_SCAN_WINDOW_MS       60     // controller scan window (< interval)
#define BLE_STALE_AFTER_MS       90000  // gray a tile unheard this long

// ---------------------------------------------------------------------------
// microSD card — CSV data logging (sdlog.cpp). The card is wired to the
// VSPI bus on its own GPIOs; the bus is uncontended (no touchscreen).
// ---------------------------------------------------------------------------
#define SD_SCLK            18
#define SD_MOSI            23
#define SD_MISO            19
#define SD_CS              5
#define SD_SPI_FREQ_HZ     20000000     // SD owns VSPI; 20 MHz is safe on the CYD
#define SD_LOG_PREFIX      "/govee-"    // dated log files: /govee-YYYY-MM.csv
#define SD_LOG_FILENAME    "/govee-log.csv"  // pre-NTP fallback (before the clock is set)
#define SD_LOG_INTERVAL_MS 300000       // append a row per sensor every 5 minutes

// NTP — UTC wall-clock for the CSV timestamp column. Falls back to
// uptime-seconds until the first sync (the LAN may have no internet).
#define NTP_SERVER         "pool.ntp.org"

// AP mode (WiFiManager fallback when no creds saved). The SSID is
// built at runtime as "<device_hostname>-setup" so multiple units being
// onboarded simultaneously don't show identical networks.
#define AP_PASSWORD              ""    // open AP; user only sees it during setup
#define AP_TIMEOUT_S             180

// Backlight auto-dim
//
// CYD wiring (per the Sunton schematic): R10 1MΩ pull-up to 3V3, LDR
// between GPIO 34 and GND. So bright light drops the LDR's resistance,
// pulls the tap toward GND, and produces a LOW raw ADC value. Dark
// produces a HIGH raw value. Constants below are named for the room
// condition, not the raw direction — so BL_LDR_BRIGHT < BL_LDR_DARK
// numerically.
//
// Values measured at ADC_6db attenuation through an enclosure cover:
// indoor-lit ≈ 170 raw, dim ≈ 300, lights-out ≈ 500. Headroom added
// at both ends so brighter-than-tested and darker-than-tested clamp
// cleanly to MAX/MIN duty.
#define BL_MAX_DUTY              255
#define BL_MIN_DUTY              30
#define BL_LDR_BRIGHT            150   // raw ADC when room is bright (low because of CYD wiring)
#define BL_LDR_DARK              550   // raw ADC when room is dark (high because of CYD wiring)
