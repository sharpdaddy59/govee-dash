// ota.h — HTTP-based OTA firmware update.
//
// Registers POST /ota/upload into the project's WebServer: a multipart
// upload of a raw .bin firmware image. The file-picker + progress UI
// lives in the settings SPA (web/index.html); this module owns only the
// upload endpoint and the flash mechanics.
//
// The CYD is a plain WROOM-32 with NO PSRAM, so each multipart chunk
// streams straight into the inactive OTA app partition — no whole-image
// buffer. Still safe: the Update library writes only the *spare*
// partition and flips the boot pointer in Update.end(); a torn or
// invalid upload leaves the running firmware bootable and untouched.

#pragma once

class WebServer;

// Register /ota/upload on the given WebServer. Call from
// http_server_begin() — the project's only WebServer lives there.
void ota_register(WebServer& server);
