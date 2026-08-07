#include "state.h"
#include <cstring>
#include <cmath>

DeviceRecord          g_devices[MAX_DEVICES];
std::atomic<uint32_t> g_state_version{0};
std::atomic<uint32_t> g_ui_config_gen{0};
SemaphoreHandle_t     g_devices_mutex = nullptr;

void state_bump_version() {
  g_state_version.fetch_add(1, std::memory_order_relaxed);
}

// Reset every field of a slot EXCEPT `valid`. Called with valid==false
// (init, fresh insert, post-remove scrub) so lock-free readers are never
// looking at the fields while they change.
static void reset_slot(DeviceRecord& d) {
  d.hostname[0] = '\0';
  d.mac[0]      = '\0';
  d.model.store((uint8_t)GoveeModel::UNKNOWN);
  d.temp_c.store(NAN);
  d.humidity.store(NAN);
  d.battery_pct.store(-1);
  d.rssi.store(0);
  d.last_seen_ms.store(0);
  d.stale.store(true);
  d.has_data.store(false);
}

void state_init() {
  g_devices_mutex = xSemaphoreCreateMutex();
  for (auto& d : g_devices) {
    d.valid.store(false);
    reset_slot(d);
  }
}

// Callers must hold g_devices_mutex (or accept a benign race — the UI's
// lock-free reads tolerate a slot flipping valid mid-scan).
static int find_valid_by_hostname(const char* hostname) {
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (g_devices[i].valid.load() &&
        strcmp(g_devices[i].hostname, hostname) == 0) return i;
  }
  return -1;
}

static int find_valid_by_mac(const char* mac) {
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (g_devices[i].valid.load() &&
        strcmp(g_devices[i].mac, mac) == 0) return i;
  }
  return -1;
}

int state_upsert(const char* hostname, const char* mac, GoveeModel model) {
  if (xSemaphoreTake(g_devices_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;

  int existing = find_valid_by_hostname(hostname);
  if (existing >= 0) { xSemaphoreGive(g_devices_mutex); return existing; }

  int slot = -1;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!g_devices[i].valid.load()) { slot = i; break; }
  }
  if (slot < 0) { xSemaphoreGive(g_devices_mutex); return -1; }  // table full

  DeviceRecord& d = g_devices[slot];
  // Fully populate while valid is still false — publish-last discipline.
  reset_slot(d);
  strncpy(d.hostname, hostname, sizeof(d.hostname) - 1);
  d.hostname[sizeof(d.hostname) - 1] = '\0';
  strncpy(d.mac, mac, sizeof(d.mac) - 1);
  d.mac[sizeof(d.mac) - 1] = '\0';
  d.model.store((uint8_t)model);
  d.valid.store(true);

  xSemaphoreGive(g_devices_mutex);
  state_bump_version();
  return slot;
}

// Retire under the mutex: valid=false FIRST (readers see the slot vanish
// atomically), then scrub the fields so the next insert starts clean.
static bool remove_idx_locked(int idx) {
  if (idx < 0) return false;
  DeviceRecord& d = g_devices[idx];
  d.valid.store(false);
  reset_slot(d);
  return true;
}

bool state_remove_by_mac(const char* mac) {
  if (xSemaphoreTake(g_devices_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  bool ok = remove_idx_locked(find_valid_by_mac(mac));
  xSemaphoreGive(g_devices_mutex);
  if (ok) state_bump_version();
  return ok;
}

bool state_remove_by_hostname(const char* hostname) {
  if (xSemaphoreTake(g_devices_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  bool ok = remove_idx_locked(find_valid_by_hostname(hostname));
  xSemaphoreGive(g_devices_mutex);
  if (ok) state_bump_version();
  return ok;
}

uint8_t state_active_count() {
  uint8_t n = 0;
  for (auto& d : g_devices) if (d.valid.load()) n++;
  return n;
}
