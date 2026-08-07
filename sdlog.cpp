// sdlog.cpp — CSV data logging to the microSD card.
//
// Every SD_LOG_INTERVAL_MS, append one CSV row per sensor to a dated
// file /govee-YYYY-MM.csv (one per local calendar month). The card is
// optional: if it is absent or the mount fails, sdlog_begin() disables
// logging and the dashboard runs normally.
//
// Runs entirely from loop() (sdlog_loop), the same task as ui_loop /
// http_server_loop — so there is no separate task and no locking. Sensor
// readings are read from g_devices' atomics, exactly as the UI and the
// /devices handler do. The SD card owns the VSPI bus outright.
//
// Logging records, never interprets. The CSV is a raw transcript of the
// same readings the screen and /devices already expose, with a time axis
// added — no code path ever reads the CSV back to make a decision.

#include "sdlog.h"
#include "config.h"
#include "state.h"
#include "prefs.h"
#include "govee_decode.h"
#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <time.h>
#include <stdlib.h>
#include <cmath>

static bool     s_active      = false;
static uint32_t s_last_log_ms = 0;

static const char* CSV_HEADER =
  "timestamp,uptime_s,hostname,alias,model,temp_c,humidity,battery_pct,"
  "rssi,stale";

// The POSIX TZ string for log timestamps — the user's pref, or "UTC0"
// when unset (which makes localtime() == UTC).
static const char* tz_posix() {
  const char* tz = prefs_timezone();
  return (tz && tz[0]) ? tz : "UTC0";
}

void sdlog_begin() {
  // The microSD card is wired to the VSPI pins. Bring VSPI up on those
  // pins (the global SPI instance is VSPI on the classic ESP32), then
  // mount the card through it.
  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, SD_SPI_FREQ_HZ)) {
    Serial.println("[sdlog] no card / mount failed — logging disabled");
    return;
  }
  Serial.printf("[sdlog] card mounted (%llu MB)\n",
                SD.cardSize() / (1024ULL * 1024ULL));

  // The log filename is the current date, not known until NTP syncs — so
  // the file is opened (and headered if new) per-cycle in sdlog_loop().
  s_active = true;
  Serial.println("[sdlog] card mounted — logging enabled");

  // Apply the configured timezone and start NTP (non-blocking).
  // configTzTime sets TZ then kicks off SNTP in one call.
  configTzTime(tz_posix(), NTP_SERVER);
}

bool sdlog_active() { return s_active; }

// Re-apply the timezone at runtime (NTP is already running) so a change
// from the web console takes effect on the next logged row.
void sdlog_apply_timezone() {
  setenv("TZ", tz_posix(), 1);
  tzset();
}

// True once the system clock looks NTP-set (any plausible post-2023
// epoch). Until then the timestamp column is left empty and uptime_s
// carries row ordering.
static bool clock_synced() {
  return time(nullptr) > 1700000000;
}

// Fill `buf` with the log path for this cycle: /govee-YYYY-MM.csv keyed
// on the LOCAL month once NTP has synced, or SD_LOG_FILENAME as the
// fallback until then. localtime_r honors the TZ set by configTzTime /
// sdlog_apply_timezone, so a file's month boundary matches the local
// date in its rows' timestamps.
static void current_log_path(char* buf, size_t len) {
  if (clock_synced()) {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(buf, len, SD_LOG_PREFIX "%Y-%m.csv", &lt);
  } else {
    strncpy(buf, SD_LOG_FILENAME, len - 1);
    buf[len - 1] = '\0';
  }
}

// Append a float cell with `decimals` places, or an empty cell for NaN
// (a NaN reading means "no data" — an empty cell, not 0, keeps a
// spreadsheet honest). Mirrors http_server.cpp's emit_float_or_null.
static void csv_float(File& f, float v, int decimals) {
  if (!isnan(v)) f.print(v, decimals);
}

// Append `s` as a quoted CSV field (the alias is user-set and may
// contain a comma); embedded double-quotes are doubled per RFC 4180.
static void csv_quoted(File& f, const char* s) {
  f.print('"');
  for (const char* p = s; *p; p++) {
    if (*p == '"') f.print('"');
    f.print(*p);
  }
  f.print('"');
}

void sdlog_loop() {
  if (!s_active) return;
  uint32_t now = millis();
  if (now - s_last_log_ms < SD_LOG_INTERVAL_MS) return;
  s_last_log_ms = now;

  if (state_active_count() == 0) return;   // nothing to log yet

  // The current month's file (a dated name once NTP has set the clock;
  // the fallback file until then). Open / append / close per interval —
  // no long-held handle, so a card yank can't corrupt the FAT and is
  // detected here.
  char path[24];
  current_log_path(path, sizeof(path));
  bool is_new = !SD.exists(path);
  File f = SD.open(path, FILE_APPEND);
  if (!f) {
    Serial.println("[sdlog] append failed — card removed? logging disabled");
    s_active = false;
    return;
  }
  if (is_new) f.println(CSV_HEADER);   // each month's file gets its own header

  // Timestamp: ISO-8601 local time once NTP has synced, else empty.
  // UTC renders with a trailing Z; any other zone with a ±HH:MM offset.
  // The offset is derived by comparing localtime to gmtime — newlib's
  // struct tm here has no tm_gmtoff field.
  char ts[32] = {0};
  if (clock_synced()) {
    time_t t = time(nullptr);
    struct tm lt, gt;
    localtime_r(&t, &lt);
    gmtime_r(&t, &gt);
    size_t len = strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &lt);

    int off_min = (lt.tm_hour * 60 + lt.tm_min) -
                  (gt.tm_hour * 60 + gt.tm_min);
    // Correct for a calendar-day rollover (the offset is always < 24 h).
    if (lt.tm_year != gt.tm_year)
      off_min += (lt.tm_year > gt.tm_year) ? 1440 : -1440;
    else if (lt.tm_yday != gt.tm_yday)
      off_min += (lt.tm_yday > gt.tm_yday) ? 1440 : -1440;

    if (off_min == 0) {
      ts[len]     = 'Z';
      ts[len + 1] = '\0';
    } else {
      int a = (off_min < 0) ? -off_min : off_min;
      snprintf(ts + len, sizeof(ts) - len, "%c%02d:%02d",
               (off_min < 0) ? '-' : '+', a / 60, a % 60);
    }
  }
  uint32_t uptime_s = now / 1000;

  for (int i = 0; i < MAX_DEVICES; i++) {
    const DeviceRecord& d = g_devices[i];
    if (!d.valid.load()) continue;

    f.print(ts);          f.print(',');
    f.print(uptime_s);    f.print(',');
    f.print(d.hostname);  f.print(',');
    // prefs_alias_for() returns a pointer into a shared static buffer —
    // consume it immediately.
    csv_quoted(f, prefs_alias_for(d.hostname));  f.print(',');
    f.print(govee_model_label((GoveeModel)d.model.load()));  f.print(',');
    csv_float(f, d.temp_c.load(),   1);  f.print(',');
    csv_float(f, d.humidity.load(), 1);  f.print(',');
    { int bp = d.battery_pct.load(); if (bp >= 0) f.print(bp); }
    f.print(',');
    f.print(d.rssi.load());  f.print(',');
    f.println(d.stale.load() ? 1 : 0);
  }

  f.flush();
  f.close();
}

// ---------------------------------------------------------------------------
// Log-file access for the HTTP layer (/logs and /logs/download).
// ---------------------------------------------------------------------------

// Path-traversal gate: true only for a safe BARE govee log filename.
// A plain C-string scan, applied to the user-supplied download param
// before any SD.open.
bool sdlog_is_log_filename(const char* name) {
  if (!name || !name[0]) return false;
  size_t n = strlen(name);
  if (n > 64) return false;
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    if (!ok) return false;                            // rejects '/', '\\', spaces…
    if (c == '.' && name[i + 1] == '.') return false; // rejects ".."
  }
  if (strncmp(name, "govee-", 6) != 0) return false;  // expected prefix
  if (n < 4 || strcmp(name + n - 4, ".csv") != 0) return false;
  return true;
}

void sdlog_list_files(void (*cb)(const char*, uint32_t, void*), void* ctx) {
  if (!s_active) return;
  File root = SD.open("/");
  if (!root) return;
  File e;
  while ((e = root.openNextFile())) {
    if (!e.isDirectory()) {
      // File::name() may be a full path or a bare name across core
      // versions — strip any leading directory to a bare filename.
      const char* nm    = e.name();
      const char* slash = strrchr(nm, '/');
      const char* base  = slash ? slash + 1 : nm;
      if (sdlog_is_log_filename(base)) cb(base, (uint32_t)e.size(), ctx);
    }
    e.close();
  }
  root.close();
}

File sdlog_open_for_read(const char* validated_name) {
  if (!s_active) return File();
  String path = "/";
  path += validated_name;
  return SD.open(path, FILE_READ);
}
