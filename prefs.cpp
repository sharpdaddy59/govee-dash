#include "prefs.h"
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <vector>
#include <string>

// Each NVS namespace is single-purpose so wiping one doesn't disturb others.
static Preferences s_ui;       // namespace "gdash-ui"
static Preferences s_aliases;  // namespace "gdash-alias"
static Preferences s_ignore;   // namespace "gdash-ignore"

// Bump this whenever the meaning of stored prefs changes in a way that
// existing values would be invalid (e.g., a panel-config change in ui.cpp
// that would make the old rotation value produce garbled output). On
// boot, mismatched schema triggers a one-time reset to safe defaults.
// Additive keys (a new getter with a sensible default) need no bump.
static constexpr uint8_t PREFS_SCHEMA = 1;

static BrightnessMode s_mode   = BRIGHTNESS_AUTO;
static uint8_t        s_rot    = 4;        // CYD landscape (panel swap + offset_y=80 in ui.cpp)
static TempUnitPref   s_units  = TEMP_UNIT_CELSIUS;
static String         s_tz;                // POSIX TZ for SD-log timestamps ("" = UTC)
static uint16_t       s_expiry = 0;        // auto-forget hours; 0 = never

// Ignore list lives in RAM for cheap per-advert lookups from the scan
// task; the HTTP handler (loop task) mutates it. The mutex covers the
// vector — NVS writes happen inside the same critical section since
// they're rare (user clicks).
static std::vector<std::string> s_ignored;
static SemaphoreHandle_t        s_ignore_mutex = nullptr;

static void load_ignored() {
  s_ignored.clear();
  s_ignore.begin("gdash-ignore", true);
  uint8_t n = s_ignore.getUChar("count", 0);
  for (uint8_t i = 0; i < n; i++) {
    char key[8];
    snprintf(key, sizeof(key), "m%u", i);
    String m = s_ignore.getString(key, "");
    if (m.length()) s_ignored.push_back(m.c_str());
  }
  s_ignore.end();
}

// Caller holds s_ignore_mutex.
static void save_ignored_locked() {
  s_ignore.begin("gdash-ignore", false);
  s_ignore.clear();
  s_ignore.putUChar("count", (uint8_t)s_ignored.size());
  for (size_t i = 0; i < s_ignored.size(); i++) {
    char key[8];
    snprintf(key, sizeof(key), "m%u", (unsigned)i);
    s_ignore.putString(key, s_ignored[i].c_str());
  }
  s_ignore.end();
}

void prefs_load() {
  if (!s_ignore_mutex) s_ignore_mutex = xSemaphoreCreateMutex();

  s_ui.begin("gdash-ui", true);
  uint8_t schema = s_ui.getUChar("schema", 0);
  s_ui.end();

  if (schema != PREFS_SCHEMA) {
    // First boot of this build (or a schema-incompatible upgrade).
    // Reset to safe defaults and write the new schema number.
    s_mode   = BRIGHTNESS_AUTO;
    s_rot    = 4;
    s_units  = TEMP_UNIT_CELSIUS;
    s_tz     = "";
    s_expiry = 0;
    s_ui.begin("gdash-ui", false);
    s_ui.putUChar("mode",    (uint8_t)s_mode);
    s_ui.putUChar("rot",     s_rot);
    s_ui.putUChar("unit",    (uint8_t)s_units);
    s_ui.putString("tz",     s_tz);
    s_ui.putUShort("expiry", s_expiry);
    s_ui.putUChar("schema",  PREFS_SCHEMA);
    s_ui.end();
  } else {
    s_ui.begin("gdash-ui", true);
    s_mode   = (BrightnessMode)s_ui.getUChar("mode", BRIGHTNESS_AUTO);
    s_rot    = s_ui.getUChar("rot", 4);
    s_units  = (TempUnitPref)s_ui.getUChar("unit", TEMP_UNIT_CELSIUS);
    s_tz     = s_ui.getString("tz", "");
    s_expiry = s_ui.getUShort("expiry", 0);
    s_ui.end();
  }
  load_ignored();
}

void prefs_save() {
  s_ui.begin("gdash-ui", false);
  s_ui.putUChar("mode",    (uint8_t)s_mode);
  s_ui.putUChar("rot",     s_rot);
  s_ui.putUChar("unit",    (uint8_t)s_units);
  s_ui.putString("tz",     s_tz);
  s_ui.putUShort("expiry", s_expiry);
  s_ui.end();
}

BrightnessMode prefs_brightness_mode()           { return s_mode; }
void           prefs_set_brightness_mode(BrightnessMode m) { s_mode = m; prefs_save(); }
uint8_t        prefs_rotation()                  { return s_rot;  }
void           prefs_set_rotation(uint8_t r)     { s_rot = r;  prefs_save(); }

TempUnitPref prefs_temp_unit() { return s_units; }
void         prefs_set_temp_unit(TempUnitPref u) {
  if (u > TEMP_UNIT_FAHRENHEIT) u = TEMP_UNIT_CELSIUS;
  s_units = u;
  prefs_save();
}

const char* prefs_timezone() { return s_tz.c_str(); }
void        prefs_set_timezone(const char* tz) {
  s_tz = tz ? tz : "";
  prefs_save();
}

uint16_t prefs_expiry_hours() { return s_expiry; }
void     prefs_set_expiry_hours(uint16_t h) {
  if (h > EXPIRY_HOURS_MAX) h = EXPIRY_HOURS_MAX;
  s_expiry = h;
  prefs_save();
}

const char* prefs_alias_for(const char* hostname) {
  static String s_buf;  // returned pointer is valid until the next call
  s_aliases.begin("gdash-alias", true);
  s_buf = s_aliases.getString(hostname, "");
  s_aliases.end();
  return s_buf.c_str();
}
void prefs_set_alias(const char* hostname, const char* alias) {
  s_aliases.begin("gdash-alias", false);
  if (alias && *alias) s_aliases.putString(hostname, alias);
  else                 s_aliases.remove(hostname);
  s_aliases.end();
}

bool prefs_is_ignored(const char* mac) {
  if (!s_ignore_mutex) return false;
  bool found = false;
  if (xSemaphoreTake(s_ignore_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  for (auto& m : s_ignored) {
    if (m == mac) { found = true; break; }
  }
  xSemaphoreGive(s_ignore_mutex);
  return found;
}

bool prefs_ignore_add(const char* mac) {
  if (xSemaphoreTake(s_ignore_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  for (auto& m : s_ignored) {
    if (m == mac) { xSemaphoreGive(s_ignore_mutex); return false; }
  }
  s_ignored.emplace_back(mac);
  save_ignored_locked();
  xSemaphoreGive(s_ignore_mutex);
  return true;
}

bool prefs_ignore_remove(const char* mac) {
  if (xSemaphoreTake(s_ignore_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  for (auto it = s_ignored.begin(); it != s_ignored.end(); ++it) {
    if (*it == mac) {
      s_ignored.erase(it);
      save_ignored_locked();
      xSemaphoreGive(s_ignore_mutex);
      return true;
    }
  }
  xSemaphoreGive(s_ignore_mutex);
  return false;
}

uint8_t prefs_ignored_count() {
  if (xSemaphoreTake(s_ignore_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
  uint8_t n = (uint8_t)s_ignored.size();
  xSemaphoreGive(s_ignore_mutex);
  return n;
}

bool prefs_ignored_mac(uint8_t i, char* out, size_t outlen) {
  bool ok = false;
  if (xSemaphoreTake(s_ignore_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  if (i < s_ignored.size()) {
    strncpy(out, s_ignored[i].c_str(), outlen - 1);
    out[outlen - 1] = '\0';
    ok = true;
  }
  xSemaphoreGive(s_ignore_mutex);
  return ok;
}
