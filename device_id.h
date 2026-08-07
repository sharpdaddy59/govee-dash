// device_id.h — per-MAC unique hostname for this dashboard.
//
// All units flash the same firmware, so a static "govee-dash" hostname
// would collide on a LAN with multiple CYDs. We derive a 4-hex suffix
// from the lower 16 bits of the MAC address, yielding e.g.
// "govee-dash-a3f2".
//
// The hostname is also used to:
//   - register mDNS (so each unit is reachable as <hostname>.local)
//   - build the WiFiManager AP SSID (so different units' setup APs are
//     distinguishable when several need to be onboarded at once)
//   - display in the grid footer (so the user can identify which CYD
//     is which when several are deployed)

#pragma once

// Returns a stable per-device hostname like "govee-dash-a3f2".
// Computed once on first call, cached. Safe to call before WiFi.begin.
const char* device_hostname();
