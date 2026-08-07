#include "wifi_setup.h"
#include "config.h"
#include "ui.h"
#include "device_id.h"
#include <WiFi.h>
#include <WiFiManager.h>

static bool s_in_ap = false;
static String s_ap_ssid;   // built once at begin, kept stable for the AP-callback closure

void wifi_setup_begin() {
  // Build a per-device AP SSID so several CYDs being onboarded at once
  // produce distinguishable networks instead of collapsing onto a single
  // "govee-dash-setup" everyone sees.
  s_ap_ssid = String(device_hostname()) + "-setup";

  WiFiManager wm;
  wm.setConfigPortalTimeout(AP_TIMEOUT_S);
  wm.setAPCallback([](WiFiManager* /*m*/) {
    s_in_ap = true;
    String msg = String("AP: ") + s_ap_ssid;
    ui_set_status(msg.c_str());
  });

  // autoConnect either reuses saved creds or opens an AP and blocks
  // until the user submits credentials. After timeout it returns false
  // and we reboot — there's nothing useful for a dashboard to do offline.
  bool ok = wm.autoConnect(s_ap_ssid.c_str());
  if (!ok) {
    ui_set_status("WiFi failed; rebooting");
    delay(2000);
    ESP.restart();
  }
  s_in_ap = false;
}

void wifi_setup_reset_and_reboot() {
  WiFiManager wm;
  wm.resetSettings();
  delay(200);
  ESP.restart();
}

bool wifi_setup_in_ap_mode() { return s_in_ap; }
