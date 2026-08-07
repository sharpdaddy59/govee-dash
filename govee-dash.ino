// govee-dash.ino — entry point.
//
// Standalone dashboard for Govee BLE hygrometer/thermometer sensors
// (H5075, H5074, H5102, H5179). Passively listens to their BLE
// advertisements — no pairing, no cloud, no app — and renders every
// sensor in range on an adaptive grid.
//
// Hardware: Sunton ESP32-2432S028R (CYD). Pin map in config.h.
// Spun off from hydro-dash with the cores3-hydro HTTP-polling half
// removed; conventions are deliberately mirrored.
//
// Boot sequence is intentionally explicit.

#include "config.h"
#include "state.h"
#include "prefs.h"
#include "backlight.h"
#include "ui.h"
#include "wifi_setup.h"
#include "ble_scanner.h"
#include "sdlog.h"
#include "http_server.h"
#include "device_id.h"
#include <ESPmDNS.h>

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.printf("[boot] govee-dash %s\n", FW_VERSION);

  // Allocate the device-table mutex before anything that reads/writes it.
  state_init();

  // NVS first — UI prefs, aliases and the ignore list inform later steps.
  prefs_load();

  // Backlight + display before WiFi so the user sees a "connecting"
  // screen during onboarding rather than a dark panel.
  backlight_begin();
  ui_begin();
  ui_set_status("Connecting to WiFi...");

  // Blocks until WiFi up. WiFiManager opens an AP if no creds saved.
  wifi_setup_begin();

  // mDNS responder — the settings page is reachable at <hostname>.local.
  if (!MDNS.begin(device_hostname())) {
    Serial.println("[boot] mDNS start failed (page still reachable by IP)");
  }

  // Passive BLE advertisement scan for Govee sensors. After WiFi so BLE
  // init doesn't contend with WiFiManager's AP-mode radio use.
  ble_scanner_begin();

  // Append sensor readings to a CSV on the microSD card, if one is in.
  sdlog_begin();

  // Settings SPA + management API — /devices, /config, /ota/upload, ...
  http_server_begin();

  ui_set_status("");  // clear the connecting message
}

void loop() {
  ui_loop();
  backlight_loop();
  sdlog_loop();
  http_server_loop();
  delay(10);
}
