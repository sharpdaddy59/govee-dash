// ble_scanner.h — passive BLE advertisement scan for Govee
// temperature/humidity sensors (H5075, H5074, H5102, H5179).
//
// Scan-only: no pairing, no GATT connect. Supported Govee models
// broadcast their readings in the manufacturer-specific data of their
// BLE advertisements; this module listens, dispatches to the per-model
// decoder (govee_decode.cpp), and surfaces each sensor as a slot in
// g_devices — rendered by the grid view.
//
// One FreeRTOS task, pinned to core 0. The scan duty cycle is higher
// than hydro-dash's (no HTTP polling to protect) but still gapped so
// WiFi is not starved (the WROOM-32 shares one radio).

#pragma once

// Brings up NimBLE (scan-only) and spawns the scan task. Call after
// wifi_setup_begin() so BLE init does not contend with WiFiManager's
// AP-mode radio use during onboarding.
void ble_scanner_begin();
