// state.h — shared sensor records + atomic flags.
//
// Convention inherited from hydro-dash: the producer (ble_scanner) writes
// into atomics; consumers (ui_grid, http_server, sdlog) read without
// locking. Mutation of slot OCCUPANCY (insert, remove) is guarded by
// g_devices_mutex.
//
// Unlike hydro-dash's append-only array, slots here are TOMBSTONED:
//   - insert populates the identity fields and resets the atomics while
//     `valid` is still false, then publishes with valid=true LAST — so a
//     lock-free reader never sees a half-populated live record;
//   - remove clears `valid` FIRST, then resets the fields. A reader
//     mid-frame sees the slot vanish; the grid's layout-epoch detection
//     triggers a clean full redraw on the next frame. Slots never move,
//     which is what keeps lock-free reads safe (no compaction).
// This fixes hydro-dash's known "dead sensor can't be removed without a
// reboot" limitation — Govee sensors come and go.

#pragma once

#include <atomic>
#include <cstring>
#include <cmath>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config.h"
#include "govee_decode.h"   // GoveeModel

struct DeviceRecord {
  // Slot occupancy — the publish/retire flag. See file header for the
  // ordering discipline (write LAST on insert, clear FIRST on remove).
  std::atomic<bool> valid;

  // Identity — written only while valid==false, under g_devices_mutex.
  char hostname[20];          // synthesized "govee-a3f2c1" (stable key)
  char mac[18];               // "AA:BB:CC:DD:EE:FF"
  std::atomic<uint8_t> model; // GoveeModel

  // Latest decoded advertisement (NaN / -1 until first decode).
  std::atomic<float>    temp_c;
  std::atomic<float>    humidity;
  std::atomic<int>      battery_pct;   // -1 until known
  std::atomic<int>      rssi;

  // Health
  std::atomic<uint32_t> last_seen_ms;  // millis() of last decoded advert
  std::atomic<bool>     stale;         // unheard > BLE_STALE_AFTER_MS
  std::atomic<bool>     has_data;      // true once any advert decoded
};

// Store v into dst only if it differs from the current value. Returns true
// iff a write happened — callers OR this into a `changed` flag and only
// bump the UI version when something visible actually moved. Without this,
// a scan window that yielded identical readings would still trigger a
// redraw, seen by the user as a periodic flash.
template <typename T>
inline bool set_if_changed(std::atomic<T>& dst, T v) {
  T old = dst.load();
  if (old == v) return false;
  dst.store(v);
  return true;
}

// NaN-aware float variant: treat NaN→NaN as no-change. Without this an
// absent reading (NaN every refresh) would register as a change on every
// comparison since NaN != NaN per IEEE 754.
inline bool set_float_if_changed(std::atomic<float>& dst, float v) {
  float old = dst.load();
  if ((isnan(old) && isnan(v)) || old == v) return false;
  dst.store(v);
  return true;
}

// Globals — defined in state.cpp.
extern DeviceRecord      g_devices[MAX_DEVICES];
extern SemaphoreHandle_t g_devices_mutex;

// Bumped whenever something display-relevant changes (new reading,
// stale-flag flip, sensor added/removed, pref change). The UI loop skips
// a redraw when the version hasn't moved since last frame — this is what
// kills idle flicker without per-element dirty-tracking.
extern std::atomic<uint32_t> g_state_version;

// Bumped when a rendering-relevant CONFIG changes (alias edit, temp-unit
// switch). The grid folds this into its layout epoch, forcing a full
// clear+redraw — value-diffing alone can't see "same temp_c, different
// suffix". Bump alongside g_state_version.
extern std::atomic<uint32_t> g_ui_config_gen;

// Helpers (state.cpp)
void state_init();
void state_bump_version();                 // invalidate UI cache

// Find-or-insert by synthesized hostname. On insert, stores mac + model
// and resets the reading atomics before publishing valid=true. Returns
// the slot index, or -1 if the table is full (all slots valid).
int  state_upsert(const char* hostname, const char* mac, GoveeModel model);

// Retire a sensor's slot by MAC or hostname. Returns true if a slot was
// removed. The slot becomes immediately reusable by the next insert.
bool state_remove_by_mac(const char* mac);
bool state_remove_by_hostname(const char* hostname);

// Number of valid slots right now (iterates all MAX_DEVICES slots).
uint8_t state_active_count();
