// http_server.h — management API + settings web app.
//
// Routes:
//   GET  /                   settings single-page app (gzipped, PROGMEM)
//   GET  /devices            JSON snapshot of known sensors
//   POST /devices/forget     remove a sensor (body: mac=... [ignore=1])
//   POST /devices/unignore   un-block an ignored sensor (body: mac=...)
//   GET  /status             uptime, FW_VERSION, heap, sensor count
//   GET  /config             settings snapshot (incl. ignored list)
//   POST /config/brightness  set backlight mode       (body: mode=...)
//   POST /config/units       set temp display unit    (body: unit=...)
//   POST /config/timezone    set SD-log timezone      (body: tz=...)
//   POST /config/expiry      set auto-forget hours    (body: hours=...)
//   POST /config/alias       set/clear a display name (body: hostname,alias)
//   GET  /logs               JSON list of SD log files
//   GET  /logs/download      stream one CSV           (?file=...)
//   POST /wifi/reset         wipe WiFi creds and reboot
//   POST /ota/upload         firmware update          (see ota.cpp)
//
// Synchronous WebServer polled from loop() via http_server_loop().
// LAN-trusted, no auth.

#pragma once

void http_server_begin();
void http_server_loop();
