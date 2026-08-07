// ble_scanner.cpp — passive BLE scan for Govee hygrometer sensors.
//
// Built against NimBLE-Arduino 2.x (pinned in setup.ps1). NimBLE 2.x is
// the line compatible with arduino-esp32 3.x / ESP-IDF 5.x — the 1.4.x
// line targets IDF 4.x and its esp_bt_controller_init() aborts at boot on
// a 3.x core. The scan API here (NimBLEScanCallbacks, const onResult,
// setScanCallbacks, getResults) is 2.x-specific.
//
// Model dispatch and byte-level decode live in govee_decode.cpp; this
// file owns the scan loop, the pending buffer, staleness, and expiry.

#include "ble_scanner.h"
#include "govee_decode.h"
#include "config.h"
#include "state.h"
#include "prefs.h"
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cmath>
#include <string>

// ---------------------------------------------------------------------------
// Debug logging — off by default. Set BLE_DEBUG to 1 to log every scan
// window and every decoded Govee advert (raw manufacturer-data hex +
// decoded values) to serial @ 115200. THIS IS THE BRING-UP TOOL for the
// unverified model layouts (H5074/H5102/H5179) — compare the decoded
// values against each sensor's own LCD, and watch for the "??" safety-net
// lines that flag a Govee-named advert our dispatch didn't match.
// ---------------------------------------------------------------------------
#define BLE_DEBUG 0

#if BLE_DEBUG
  #define BLE_LOGF(...) Serial.printf(__VA_ARGS__)
// Plain counters — incremented only from the NimBLE host task (onResult),
// read+reset from the scan task. A rare off-by-one in the summary is
// harmless; not worth an atomic for throwaway debug scaffolding.
static uint32_t s_dbg_total_adverts = 0;   // all adverts seen this window
static uint32_t s_dbg_govee_adverts = 0;   // decoder matches this window
static void dbg_print_hex(const std::string& s) {
  for (size_t i = 0; i < s.size(); i++) Serial.printf("%02X", (uint8_t)s[i]);
}
#else
  #define BLE_LOGF(...) ((void)0)
#endif

// ---------------------------------------------------------------------------
// Pending-results buffer — the NimBLE callback (host task) stashes here;
// the scan task drains it. The short callback never touches
// g_devices_mutex or NVS.
// ---------------------------------------------------------------------------

struct PendingSensor {
  uint8_t      mac[6];     // canonical (display) octet order
  GoveeReading reading;
  GoveeModel   model;
  int          rssi;
  bool         used;
};

static PendingSensor     s_pending[MAX_DEVICES];
static SemaphoreHandle_t s_pending_mutex = nullptr;

// NimBLE stores BLE addresses little-endian (over-the-air order); flip to
// the human-readable big-endian octet order so the synthesized hostname
// and the displayed MAC string agree.
static void canon_mac(const NimBLEAddress& addr, uint8_t out[6]) {
  const uint8_t* val = addr.getVal();
  for (int i = 0; i < 6; i++) out[i] = val[5 - i];
}

class GoveeScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* adv) override {
    // Runs on the NimBLE host task — keep it short.
#if BLE_DEBUG
    s_dbg_total_adverts++;
#endif
    if (!adv->haveManufacturerData()) return;
    std::string md = adv->getManufacturerData();
    if (md.size() < 8) return;   // smallest supported: 2-byte CID + 6-byte payload

    uint16_t cid = (uint8_t)md[0] | ((uint16_t)(uint8_t)md[1] << 8);

#if BLE_DEBUG
    // Safety net: an advert whose name looks like a Govee ("GV..." /
    // "Govee...") but that our dispatch will NOT match — either an
    // unknown company ID or (checked again below) a length mismatch on
    // a known CID. Surfaces a wrong table entry in the log instead of
    // it silently producing zero results.
    if (!govee_cid_known(cid) && adv->haveName() &&
        strncmp(adv->getName().c_str(), "GV", 2) == 0) {
      BLE_LOGF("[ble] ?? name=%s unknown company=0x%04X hex=",
               adv->getName().c_str(), cid);
      dbg_print_hex(md);
      BLE_LOGF("\n");
    }
#endif

    if (!govee_cid_known(cid)) return;

    // Opportunistic name guard. A sensor's local name may live in a scan
    // response we never see during a passive scan, so a MISSING name is
    // fine — but a present name that doesn't look like a Govee's
    // ("GVH5075_XXXX", "Govee_H5179_XXXX", ...) means some other product
    // that happens to share a company ID. The (cid, length) dispatch
    // plus the sanity gate do the real filtering; this is belt-and-
    // suspenders, deliberately loose so model-name variants pass.
    if (adv->haveName()) {
      const char* nm = adv->getName().c_str();
      if (strncmp(nm, "GV", 2) != 0 && strncmp(nm, "Govee", 5) != 0) return;
    }

    GoveeModel   model;
    GoveeReading r = govee_decode(cid, (const uint8_t*)md.data() + 2,
                                  md.size() - 2, &model);

    uint8_t canon[6];
    canon_mac(adv->getAddress(), canon);

#if BLE_DEBUG
    if (!r.ok && adv->haveName() &&
        strncmp(adv->getName().c_str(), "GV", 2) == 0) {
      // Known CID but no (cid, length) table match or implausible decode
      // — this is how a wrong payload_len assumption surfaces.
      BLE_LOGF("[ble] ?? name=%s company=0x%04X len=%u unmatched/insane hex=",
               adv->getName().c_str(), cid, (unsigned)(md.size() - 2));
      dbg_print_hex(md);
      BLE_LOGF("\n");
    }
    if (r.ok) {
      s_dbg_govee_adverts++;
      BLE_LOGF("[ble] %s mac=%02X:%02X:%02X:%02X:%02X:%02X rssi=%d name=%s hex=",
               govee_model_label(model),
               canon[0], canon[1], canon[2], canon[3], canon[4], canon[5],
               adv->getRSSI(),
               adv->haveName() ? adv->getName().c_str() : "(none)");
      dbg_print_hex(md);
      BLE_LOGF("\n[ble]   -> temp=%.2fC hum=%.1f%% batt=%d\n",
               r.temp_c, r.humidity, r.battery_pct);
    }
#endif

    if (!r.ok) return;

    if (xSemaphoreTake(s_pending_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return;
    int slot = -1;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (s_pending[i].used && memcmp(s_pending[i].mac, canon, 6) == 0) {
        slot = i;  break;
      }
    }
    if (slot < 0) {
      for (int i = 0; i < MAX_DEVICES; i++) {
        if (!s_pending[i].used) { slot = i; break; }
      }
    }
    if (slot >= 0) {
      memcpy(s_pending[slot].mac, canon, 6);
      s_pending[slot].reading = r;
      s_pending[slot].model   = model;
      s_pending[slot].rssi    = adv->getRSSI();
      s_pending[slot].used    = true;
    }
    xSemaphoreGive(s_pending_mutex);
  }
};

// ---------------------------------------------------------------------------
// Scan task — drain pending results into g_devices, age stale tiles,
// expire long-dead ones.
// ---------------------------------------------------------------------------

// "govee-a3f2c1" from the last 3 MAC octets — 16.7M-space uniqueness,
// ample for a home LAN, and stable across reboots (these sensors use a
// fixed public address). Used as the find-or-insert key, so repeated
// sightings map to the same DeviceRecord.
static void synth_hostname(const uint8_t* canon, char* out, size_t outlen) {
  snprintf(out, outlen, "govee-%02x%02x%02x", canon[3], canon[4], canon[5]);
}

static void apply_pending() {
  PendingSensor snap[MAX_DEVICES];
  if (xSemaphoreTake(s_pending_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  memcpy(snap, s_pending, sizeof(snap));
  for (auto& p : s_pending) p.used = false;   // reset for the next window
  xSemaphoreGive(s_pending_mutex);

  for (auto& p : snap) {
    if (!p.used) continue;

    char host[20], mac[18];
    synth_hostname(p.mac, host, sizeof(host));
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             p.mac[0], p.mac[1], p.mac[2], p.mac[3], p.mac[4], p.mac[5]);

    // User-blocked sensors never (re-)enter the table. Checked here in
    // the scan task (RAM lookup, mutex-guarded in prefs) rather than in
    // the time-sensitive NimBLE callback.
    if (prefs_is_ignored(mac)) continue;

    int idx = state_upsert(host, mac, p.model);   // idempotent on repeats
    if (idx < 0) {                                // MAX_DEVICES full
      BLE_LOGF("[ble] %s dropped — device table full (MAX_DEVICES)\n", host);
      continue;
    }
    DeviceRecord& d = g_devices[idx];

    // Write through set_*_if_changed so the UI version only bumps when a
    // value actually moved — these sensors re-broadcast an unchanged
    // reading every couple of seconds and that must not flash the grid.
    bool changed = false;
    changed |= set_float_if_changed(d.temp_c,   p.reading.temp_c);
    changed |= set_float_if_changed(d.humidity, p.reading.humidity);
    changed |= set_if_changed(d.rssi,        p.rssi);
    changed |= set_if_changed(d.battery_pct, p.reading.battery_pct);
    changed |= set_if_changed(d.model,       (uint8_t)p.model);

    d.last_seen_ms.store(millis());             // drives staleness + expiry
    if (!d.has_data.exchange(true)) changed = true;
    if (d.stale.exchange(false))    changed = true;

    if (changed) state_bump_version();

    BLE_LOGF("[ble] %s idx=%d %s temp=%.2fC hum=%.1f%% batt=%d rssi=%d%s\n",
             host, idx, govee_model_label(p.model), p.reading.temp_c,
             p.reading.humidity, p.reading.battery_pct, p.rssi,
             changed ? " (changed)" : "");
  }
}

// Staleness + auto-expiry owner. A tile unheard for BLE_STALE_AFTER_MS
// goes gray; one unheard past the user's expiry pref (hours, 0=never) is
// removed from the table entirely — it re-inserts within seconds if the
// sensor comes back into range. Expiry only ever removes already-stale
// records, so live data never pops off the screen.
static void age_ble_devices() {
  uint32_t now       = millis();
  uint32_t expiry_ms = (uint32_t)prefs_expiry_hours() * 3600000UL;
  for (int i = 0; i < MAX_DEVICES; i++) {
    DeviceRecord& d = g_devices[i];
    if (!d.valid.load() || !d.has_data.load()) continue;
    uint32_t unheard = now - d.last_seen_ms.load();
    if (expiry_ms > 0 && unheard > expiry_ms) {
      char mac[18];
      strncpy(mac, d.mac, sizeof(mac));
      mac[sizeof(mac) - 1] = '\0';
      BLE_LOGF("[ble] %s expired (unheard %lu min) — removing\n",
               d.hostname, (unsigned long)(unheard / 60000UL));
      state_remove_by_mac(mac);
      continue;
    }
    if (unheard > BLE_STALE_AFTER_MS) {
      if (!d.stale.exchange(true)) state_bump_version();  // bump on the 0->1 edge only
    }
  }
}

static void task_ble_scan(void* /*arg*/) {
  NimBLEScan* scan = NimBLEDevice::getScan();
  for (;;) {
    // Blocking scan window; callbacks fire on the NimBLE host task.
    // getResults(durationMs, isContinue) starts the scan, blocks, returns.
    scan->getResults((uint32_t)BLE_SCAN_DURATION_S * 1000, false);
    scan->clearResults();                     // free the result cache
    apply_pending();
    age_ble_devices();
#if BLE_DEBUG
    BLE_LOGF("[ble] window done: %u advert(s) seen, %u Govee match(es)\n",
             (unsigned)s_dbg_total_adverts, (unsigned)s_dbg_govee_adverts);
    s_dbg_total_adverts = 0;
    s_dbg_govee_adverts = 0;
#endif
    vTaskDelay(pdMS_TO_TICKS(BLE_SCAN_GAP_MS));  // WiFi-coexistence breather
  }
}

void ble_scanner_begin() {
  s_pending_mutex = xSemaphoreCreateMutex();
  for (auto& p : s_pending) p.used = false;

  NimBLEDevice::init("");                     // empty name — scan-only, never advertises
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(new GoveeScanCallbacks(), /*wantDuplicates=*/true);
  scan->setActiveScan(false);                 // passive: never transmits scan requests
  // Interval/window (milliseconds in NimBLE 2.x). Window < interval keeps
  // guaranteed clear air for WiFi on the shared radio — see the reasoning
  // block in config.h before touching these.
  scan->setInterval(BLE_SCAN_INTERVAL_MS);
  scan->setWindow(BLE_SCAN_WINDOW_MS);

  // Core 0; the latency-sensitive UI render loop owns core 1. 4 KB stack
  // matches hydro-dash's proven sizing — bump if it overflows in test.
  xTaskCreatePinnedToCore(task_ble_scan, "ble_scan", 4096, nullptr, 1, nullptr, 0);
  BLE_LOGF("[ble] scanner started — passive scan for Govee sensors\n");
}
