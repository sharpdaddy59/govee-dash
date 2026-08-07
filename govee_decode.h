// govee_decode.h — per-model Govee BLE advertisement decoders.
//
// Pure functions, no Arduino/NimBLE dependencies — testable off-device.
// The scan callback (ble_scanner.cpp) hands govee_decode() the parsed
// manufacturer company ID plus the payload with the 2-byte company ID
// ALREADY stripped; dispatch keys on (company ID, exact payload length).
// Length is load-bearing: the H5074 and H5075 share company ID 0xEC88
// and differ only in payload length (7 vs 6 bytes).
//
// Wire formats are documented in docs/govee-ble.md. Only the H5075
// layout is hardware-verified (inherited from hydro-dash); H5074, H5102,
// and H5179 are literature-derived (Home Assistant govee-ble parser,
// Theengs decoder) and must be confirmed with BLE_DEBUG on first
// bring-up. Every decoder ends in the same sanity gate, so a wrong
// layout fails safe (no tile) instead of painting garbage.

#pragma once

#include <stdint.h>
#include <stddef.h>

enum class GoveeModel : uint8_t {
  H5075 = 0,
  H5074,
  H5102,
  H5179,
  UNKNOWN,
};

struct GoveeReading {
  float temp_c;
  float humidity;
  int   battery_pct;
  bool  ok;
};

// True if `cid` (parsed little-endian: md[0] | md[1]<<8) belongs to any
// supported model — the scan callback's cheap first-pass filter.
bool govee_cid_known(uint16_t cid);

// Dispatch on (cid, len) and decode. On no table match, or a decode that
// fails the sanity gate, returns .ok=false and *out_model=UNKNOWN.
// `payload` points past the 2-byte company ID; `len` is the remaining
// byte count.
GoveeReading govee_decode(uint16_t cid, const uint8_t* payload, size_t len,
                          GoveeModel* out_model);

// "H5075", "H5074", ... — for the /devices JSON, CSV model column, and
// the on-screen model badge. Returns "?" for UNKNOWN.
const char* govee_model_label(GoveeModel m);
