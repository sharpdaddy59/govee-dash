// prefs.h — NVS-backed user preferences.
//
// Stores: brightness mode, screen rotation, temperature display unit,
// SD-log timezone, auto-forget expiry, per-sensor aliases, and the
// ignored-sensor MAC blocklist.
//
// Namespaces are fresh (gdash-*) — govee-dash shares hardware with
// hydro-dash but never its NVS data, so reflashing between the two
// firmwares can't cross-contaminate settings.

#pragma once

#include <Arduino.h>

enum BrightnessMode : uint8_t {
  BRIGHTNESS_AUTO = 0,
  BRIGHTNESS_FULL = 1,
  BRIGHTNESS_DIM  = 2,
};

// Temperature display unit. Govee sensors always broadcast Celsius on
// the wire, so unlike hydro-dash there is no "auto/mirror" mode — the
// pref is simply which unit to render.
enum TempUnitPref : uint8_t {
  TEMP_UNIT_CELSIUS    = 0,
  TEMP_UNIT_FAHRENHEIT = 1,
};

void prefs_load();
void prefs_save();

BrightnessMode prefs_brightness_mode();
void           prefs_set_brightness_mode(BrightnessMode m);

uint8_t prefs_rotation();   // LovyanGFX setRotation value (default 4)
void    prefs_set_rotation(uint8_t r);

TempUnitPref prefs_temp_unit();
void         prefs_set_temp_unit(TempUnitPref u);

// POSIX TZ string for the SD-log CSV timestamp ("" = UTC). The value is
// a POSIX TZ string (e.g. "EST5EDT,M3.2.0,M11.1.0") so DST is automatic.
const char* prefs_timezone();
void        prefs_set_timezone(const char* tz);

// Auto-forget expiry: a sensor unheard this many hours is removed from
// the device table automatically (it re-inserts if it comes back into
// range). 0 = never. Clamped to [0, EXPIRY_HOURS_MAX].
static constexpr uint16_t EXPIRY_HOURS_MAX = 168;   // one week
uint16_t prefs_expiry_hours();
void     prefs_set_expiry_hours(uint16_t h);

// SD-log append cadence in minutes. sdlog_loop() re-reads this every
// pass, so a change takes effect without a reboot. Clamped to
// [1, LOG_INTERVAL_MAX_MIN]; default SD_LOG_INTERVAL_DEFAULT_MIN.
static constexpr uint16_t LOG_INTERVAL_MAX_MIN = 1440;   // one day
uint16_t prefs_log_interval_min();
void     prefs_set_log_interval_min(uint16_t m);

// Per-sensor alias ("Greenhouse" instead of govee-a3f2c1), keyed on the
// synthesized hostname. NOTE: returns a pointer into a shared static
// buffer valid only until the next call — consume immediately (wrap in
// String for ArduinoJson).
const char* prefs_alias_for(const char* hostname);  // "" if none
void        prefs_set_alias(const char* hostname, const char* alias);

// Ignored-sensor blocklist by MAC ("AA:BB:CC:DD:EE:FF"). The BLE scan
// task checks prefs_is_ignored() before inserting a heard sensor, so an
// ignored device never reappears until unignored. Reads and mutations
// are internally mutex-guarded (scan task vs. HTTP handler on the loop
// task).
bool    prefs_is_ignored(const char* mac);
bool    prefs_ignore_add(const char* mac);      // false if already listed
bool    prefs_ignore_remove(const char* mac);   // false if not listed
uint8_t prefs_ignored_count();
// Copies entry i into out (size >= 18). Returns false if i out of range.
bool    prefs_ignored_mac(uint8_t i, char* out, size_t outlen);
