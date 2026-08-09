#include "http_server.h"
#include "config.h"
#include "state.h"
#include "prefs.h"
#include "wifi_setup.h"
#include "device_id.h"
#include "govee_decode.h"
#include "web_assets.h"
#include "ota.h"
#include "sdlog.h"
#include <WebServer.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <cmath>

static WebServer s_server(80);

static void send_json(int code, const JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  s_server.send(code, "application/json", out);
}

// Standard reply for the SPA's write endpoints — it fetch()es these and
// only checks the HTTP status, so a tiny JSON body is all that's needed.
static void send_ok() {
  s_server.send(200, "application/json", "{\"ok\":true}");
}

// GET / — the settings single-page app. Served straight from PROGMEM as
// pre-gzipped bytes (see web_assets.h, generated from web/index.html).
static void handle_root() {
  s_server.sendHeader("Content-Encoding", "gzip");
  s_server.sendHeader("Cache-Control", "no-cache");
  s_server.send_P(200, "text/html", (PGM_P)WEB_INDEX_HTML_GZ,
                  WEB_INDEX_HTML_GZ_LEN);
}

// Emit a float as JSON null if NaN (null means "no reading yet").
static void emit_float_or_null(JsonObject& o, const char* k, float v) {
  if (isnan(v)) o[k] = nullptr;
  else          o[k] = v;
}

static void handle_devices_get() {
  // Static (not stack) — at 8 sensors this document is several KB and
  // the WebServer handlers run on the main loop task's modest stack.
  // to<JsonArray>() clears it, so there's no stale carry-over.
  static StaticJsonDocument<4096> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < MAX_DEVICES; i++) {
    const DeviceRecord& d = g_devices[i];
    if (!d.valid.load()) continue;
    JsonObject o = arr.createNestedObject();
    o["hostname"] = d.hostname;
    // prefs_alias_for() returns a pointer into a shared static buffer
    // that the next call overwrites — wrap in String so ArduinoJson
    // copies the value now instead of storing the soon-stale pointer.
    o["alias"] = String(prefs_alias_for(d.hostname));
    o["mac"]   = d.mac;
    o["model"] = govee_model_label((GoveeModel)d.model.load());
    emit_float_or_null(o, "temp_c",   d.temp_c.load());
    emit_float_or_null(o, "humidity", d.humidity.load());
    int bp = d.battery_pct.load();
    if (bp >= 0) o["battery_pct"] = bp;
    else         o["battery_pct"] = nullptr;
    o["rssi"] = d.rssi.load();
    uint32_t seen = d.last_seen_ms.load();
    if (seen > 0) o["last_seen_s"] = (uint32_t)((millis() - seen) / 1000);
    else          o["last_seen_s"] = nullptr;
    o["stale"] = (bool)d.stale.load();
  }
  send_json(200, doc);
}

// POST /devices/forget — retire a sensor's slot. Body: mac=AA:BB:... (or
// hostname=govee-xxxxxx), optional ignore=1 to also blocklist the MAC so
// a still-broadcasting sensor doesn't reappear seconds later.
static void handle_devices_forget() {
  String mac;
  bool removed = false;
  if (s_server.hasArg("mac")) {
    mac = s_server.arg("mac");
    removed = state_remove_by_mac(mac.c_str());
  } else if (s_server.hasArg("hostname")) {
    // Resolve the MAC before removal so ignore=1 still works.
    String host = s_server.arg("hostname");
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (g_devices[i].valid.load() &&
          host == g_devices[i].hostname) { mac = g_devices[i].mac; break; }
    }
    removed = mac.length() && state_remove_by_mac(mac.c_str());
  } else {
    s_server.send(400, "text/plain", "need mac or hostname");
    return;
  }
  if (!removed) {
    s_server.send(404, "text/plain", "unknown sensor");
    return;
  }
  if (s_server.hasArg("ignore") && s_server.arg("ignore") == "1") {
    prefs_ignore_add(mac.c_str());
  }
  send_ok();
}

// POST /devices/unignore — body: mac=... Removes the MAC from the
// blocklist; a still-broadcasting sensor reappears within a scan window.
static void handle_devices_unignore() {
  if (!s_server.hasArg("mac")) {
    s_server.send(400, "text/plain", "need mac");
    return;
  }
  if (!prefs_ignore_remove(s_server.arg("mac").c_str())) {
    s_server.send(404, "text/plain", "not ignored");
    return;
  }
  send_ok();
}

static void handle_status() {
  StaticJsonDocument<256> doc;
  doc["fw_version"] = FW_VERSION;
  doc["uptime_s"]   = (uint32_t)(millis() / 1000);
  doc["heap_free"]  = ESP.getFreeHeap();
  doc["devices"]    = state_active_count();
  send_json(200, doc);
}

// GET /config — settings snapshot for the SPA.
static void handle_config_get() {
  StaticJsonDocument<1024> doc;
  doc["fw_version"] = FW_VERSION;
  doc["hostname"]   = device_hostname();

  const char* bl = "auto";
  switch (prefs_brightness_mode()) {
    case BRIGHTNESS_FULL: bl = "full"; break;
    case BRIGHTNESS_DIM:  bl = "dim";  break;
    case BRIGHTNESS_AUTO: break;
  }
  doc["brightness"] = bl;
  doc["temp_unit"]  = (prefs_temp_unit() == TEMP_UNIT_FAHRENHEIT)
                          ? "fahrenheit" : "celsius";
  doc["sdlog"]            = sdlog_active() ? "logging" : "no card";
  doc["timezone"]         = prefs_timezone();
  doc["expiry_hours"]     = prefs_expiry_hours();
  doc["log_interval_min"] = prefs_log_interval_min();

  JsonArray ig = doc.createNestedArray("ignored");
  uint8_t n = prefs_ignored_count();
  for (uint8_t i = 0; i < n; i++) {
    char mac[18];
    if (prefs_ignored_mac(i, mac, sizeof(mac))) ig.add(String(mac));
  }

  send_json(200, doc);
}

// POST /config/brightness — form arg mode=auto|full|dim. backlight_loop
// re-reads the mode every 500 ms, so the change applies on its own.
static void handle_config_brightness() {
  if (!s_server.hasArg("mode")) {
    s_server.send(400, "text/plain", "need mode");
    return;
  }
  String m = s_server.arg("mode");
  if      (m == "auto") prefs_set_brightness_mode(BRIGHTNESS_AUTO);
  else if (m == "full") prefs_set_brightness_mode(BRIGHTNESS_FULL);
  else if (m == "dim")  prefs_set_brightness_mode(BRIGHTNESS_DIM);
  else { s_server.send(400, "text/plain", "bad mode"); return; }
  send_ok();
}

// POST /config/units — form arg unit=celsius|fahrenheit. Config-gen bump
// forces a full grid redraw (the suffix changes even when temp_c didn't).
static void handle_config_units() {
  if (!s_server.hasArg("unit")) {
    s_server.send(400, "text/plain", "need unit");
    return;
  }
  String u = s_server.arg("unit");
  if      (u == "celsius")    prefs_set_temp_unit(TEMP_UNIT_CELSIUS);
  else if (u == "fahrenheit") prefs_set_temp_unit(TEMP_UNIT_FAHRENHEIT);
  else { s_server.send(400, "text/plain", "bad unit"); return; }
  g_ui_config_gen.fetch_add(1, std::memory_order_relaxed);
  state_bump_version();
  send_ok();
}

// POST /config/timezone — form arg tz=<POSIX TZ string> ("" = UTC). The
// value comes from the SPA's fixed zone dropdown; applied immediately so
// the next logged CSV row picks it up.
static void handle_config_timezone() {
  String tz = s_server.hasArg("tz") ? s_server.arg("tz") : String();
  prefs_set_timezone(tz.c_str());
  sdlog_apply_timezone();
  send_ok();
}

// POST /config/expiry — form arg hours=N (0 = never). The BLE scan
// task's aging pass reads the pref each window, so it applies on its own.
static void handle_config_expiry() {
  if (!s_server.hasArg("hours")) {
    s_server.send(400, "text/plain", "need hours");
    return;
  }
  long v = s_server.arg("hours").toInt();
  if (v < 0) v = 0;
  if (v > EXPIRY_HOURS_MAX) v = EXPIRY_HOURS_MAX;
  prefs_set_expiry_hours((uint16_t)v);
  send_ok();
}

// POST /config/loginterval — form arg minutes=N. sdlog_loop re-reads the
// pref every pass, so the new cadence applies without a reboot.
static void handle_config_loginterval() {
  if (!s_server.hasArg("minutes")) {
    s_server.send(400, "text/plain", "need minutes");
    return;
  }
  long v = s_server.arg("minutes").toInt();
  if (v < 1) v = 1;
  if (v > LOG_INTERVAL_MAX_MIN) v = LOG_INTERVAL_MAX_MIN;
  prefs_set_log_interval_min((uint16_t)v);
  send_ok();
}

// POST /config/alias — form args hostname=... & alias=... (empty alias
// clears it). Config-gen bump forces a full grid redraw with the new name.
static void handle_config_alias() {
  if (!s_server.hasArg("hostname")) {
    s_server.send(400, "text/plain", "need hostname");
    return;
  }
  String h = s_server.arg("hostname");
  String a = s_server.hasArg("alias") ? s_server.arg("alias") : String();
  a.trim();
  prefs_set_alias(h.c_str(), a.c_str());
  g_ui_config_gen.fetch_add(1, std::memory_order_relaxed);
  state_bump_version();
  send_ok();
}

// GET /logs — JSON list of the CSV log files on the SD card (name + size).
struct LogListCtx { JsonArray* arr; int count; };
static void log_list_cb(const char* name, uint32_t size, void* ctx) {
  LogListCtx* c = (LogListCtx*)ctx;
  if (c->count >= 180) return;          // bound the JSON document
  JsonObject o = c->arr->createNestedObject();
  o["name"] = String(name);             // copy now — `name` is transient
  o["size"] = size;
  c->count++;
}
static void handle_logs_get() {
  static StaticJsonDocument<8192> doc;
  JsonArray arr = doc.to<JsonArray>();
  LogListCtx ctx{ &arr, 0 };
  sdlog_list_files(log_list_cb, &ctx);   // empty array = no card / no files
  send_json(200, doc);
}

// GET /logs/download?file=NAME — stream one log file as a CSV download.
// `file` is untrusted: sdlog_is_log_filename() is the path-traversal gate
// and runs before any SD access.
static void handle_logs_download() {
  if (!s_server.hasArg("file")) {
    s_server.send(400, "text/plain", "need file");
    return;
  }
  String name = s_server.arg("file");
  if (!sdlog_is_log_filename(name.c_str())) {
    s_server.send(400, "text/plain", "bad file");
    return;
  }
  File f = sdlog_open_for_read(name.c_str());
  if (!f) {
    s_server.send(404, "text/plain", "not found");
    return;
  }
  s_server.sendHeader("Content-Disposition",
                      "attachment; filename=\"" + name + "\"");
  s_server.streamFile(f, "text/csv");    // blocks the loop for the transfer
  f.close();
}

void http_server_begin() {
  s_server.on("/",                  HTTP_GET,  handle_root);
  s_server.on("/devices",           HTTP_GET,  handle_devices_get);
  s_server.on("/devices/forget",    HTTP_POST, handle_devices_forget);
  s_server.on("/devices/unignore",  HTTP_POST, handle_devices_unignore);
  s_server.on("/status",            HTTP_GET,  handle_status);
  s_server.on("/config",            HTTP_GET,  handle_config_get);
  s_server.on("/config/brightness", HTTP_POST, handle_config_brightness);
  s_server.on("/config/units",      HTTP_POST, handle_config_units);
  s_server.on("/config/timezone",   HTTP_POST, handle_config_timezone);
  s_server.on("/config/expiry",     HTTP_POST, handle_config_expiry);
  s_server.on("/config/loginterval", HTTP_POST, handle_config_loginterval);
  s_server.on("/config/alias",      HTTP_POST, handle_config_alias);
  s_server.on("/logs",              HTTP_GET,  handle_logs_get);
  s_server.on("/logs/download",     HTTP_GET,  handle_logs_download);
  s_server.on("/wifi/reset", HTTP_POST, []() {
    s_server.send(200, "text/plain",
                  "Resetting WiFi - rebooting into the setup access point.");
    delay(500);
    wifi_setup_reset_and_reboot();
  });
  // POST /ota/upload — browser-driven firmware update. Owned by ota.cpp.
  ota_register(s_server);
  s_server.begin();
}

void http_server_loop() { s_server.handleClient(); }
