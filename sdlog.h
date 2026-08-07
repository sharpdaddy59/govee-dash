// sdlog.h — append sensor readings to a CSV file on the microSD card.
//
// Optional feature: if no card is present (or the mount fails) logging
// is silently disabled and the rest of the dashboard is unaffected.
// Driven from loop() — see sdlog.cpp for why no task and no locking are
// needed.

#pragma once

#include <FS.h>   // fs::File — for sdlog_open_for_read

// Mount the card. Safe to call with no card — logging just stays
// disabled. Also starts NTP for wall-clock timestamps.
void sdlog_begin();

// Append one CSV row per sensor every SD_LOG_INTERVAL_MS. Call from loop().
void sdlog_loop();

// True once a card is mounted and logging is active.
bool sdlog_active();

// Re-apply the timezone preference (called when it changes via the web
// console) so the next logged timestamp uses it.
void sdlog_apply_timezone();

// --- Log-file access for the HTTP layer (all SD/FS knowledge lives here) ---

// True if `name` (a BARE filename — no path) is a safe govee log file:
// no separators, no "..", whitelisted chars only, "govee-" prefix,
// ".csv" suffix. Apply before any SD.open of a user-supplied name.
bool sdlog_is_log_filename(const char* name);

// Enumerate the log files on the card, invoking cb(name, size, ctx) for
// each. `name` is a bare filename valid only for the duration of the
// call. No-op when no card is mounted.
void sdlog_list_files(void (*cb)(const char* name, uint32_t size, void* ctx),
                      void* ctx);

// Open a log file for reading by its (already validated) bare name.
// Returns a falsy File if no card is mounted or the file is absent.
File sdlog_open_for_read(const char* validated_name);
