// ota.cpp — HTTP OTA firmware update, streamed straight to flash.
//
// Endpoint:
//   POST /ota/upload  — multipart upload of a raw .bin firmware image
//
// The WebServer's multipart onUpload hook hands us the firmware bytes a
// chunk at a time (the multipart envelope already stripped). Each chunk
// is fed to the Update library, which writes the inactive OTA app
// partition. On a clean UPLOAD_FILE_END, Update.end(true) validates the
// image and flips the boot partition; the completion handler then
// reboots into it.
//
// No PSRAM on the CYD, so there is no whole-image buffer — we stream. A
// failure at any chunk records s_error and aborts the Update; the
// running firmware is never touched until end() flips the boot pointer,
// so an interrupted upload is a safe no-op.
//
// The WebServer is synchronous (polled from loop()), so both the chunk
// and completion handlers run on the loop task — plain statics are safe,
// no atomics needed. handleClient() blocks for the whole upload, which
// freezes the UI for its duration; acceptable for a ~10-30 s operation
// that ends in a reboot anyway.

#include <Arduino.h>
#include <WebServer.h>
#include <Update.h>

#include "ota.h"
#include "config.h"

static WebServer* s_server = nullptr;
static String     s_error;            // empty = no error so far

// --- Firmware identity check -----------------------------------------------
// The uploaded image must contain FW_ID_MARKER (config.h) somewhere in
// its bytes. This constant is that marker's home in the running image:
// referencing it here is what embeds it, so every govee-dash .bin
// carries it and a .bin from any other project doesn't.
static const char   FWID[]   = FW_ID_MARKER;
static const size_t FWID_LEN = sizeof(FWID) - 1;
static bool         s_fwid_found = false;
// Last FWID_LEN-1 stream bytes, so a marker split across two upload
// chunks is still seen.
static uint8_t      s_fwid_tail[sizeof(FWID) - 2];
static size_t       s_fwid_tail_len = 0;

static bool fwid_in(const uint8_t* b, size_t n) {
  for (size_t i = 0; n >= FWID_LEN && i <= n - FWID_LEN; i++)
    if (b[i] == FWID[0] && memcmp(b + i, FWID, FWID_LEN) == 0) return true;
  return false;
}

static void fwid_scan(const uint8_t* buf, size_t len) {
  if (s_fwid_found || len == 0) return;
  // A match spanning the chunk boundary lies entirely inside
  // (previous tail + first FWID_LEN-1 bytes of this chunk).
  if (s_fwid_tail_len) {
    uint8_t win[2 * (sizeof(FWID) - 2)];
    size_t head = len < FWID_LEN - 1 ? len : FWID_LEN - 1;
    memcpy(win, s_fwid_tail, s_fwid_tail_len);
    memcpy(win + s_fwid_tail_len, buf, head);
    if (fwid_in(win, s_fwid_tail_len + head)) { s_fwid_found = true; return; }
  }
  if (fwid_in(buf, len)) { s_fwid_found = true; return; }
  // Carry the last FWID_LEN-1 bytes of (tail + chunk) forward.
  if (len >= FWID_LEN - 1) {
    memcpy(s_fwid_tail, buf + len - (FWID_LEN - 1), FWID_LEN - 1);
    s_fwid_tail_len = FWID_LEN - 1;
  } else {
    size_t total = s_fwid_tail_len + len;
    if (total > FWID_LEN - 1) {
      size_t drop = total - (FWID_LEN - 1);
      memmove(s_fwid_tail, s_fwid_tail + drop, s_fwid_tail_len - drop);
      s_fwid_tail_len -= drop;
    }
    memcpy(s_fwid_tail + s_fwid_tail_len, buf, len);
    s_fwid_tail_len += len;
  }
}

// POST /ota/upload — chunk handler, called repeatedly during the upload.
static void handle_upload_chunk() {
  HTTPUpload& up = s_server->upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      s_error = "";
      s_fwid_found = false;
      s_fwid_tail_len = 0;
      Serial.printf("[ota] upload start: \"%s\"\n", up.filename.c_str());
      // UPDATE_SIZE_UNKNOWN: size the write against the whole spare app
      // partition. Update enforces the real image length internally.
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        s_error = Update.errorString();
        Serial.printf("[ota] begin failed: %s\n", s_error.c_str());
      }
      break;

    case UPLOAD_FILE_WRITE:
      if (s_error.length()) return;
      fwid_scan(up.buf, up.currentSize);
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        s_error = Update.errorString();
        Serial.printf("[ota] write failed: %s\n", s_error.c_str());
      }
      break;

    case UPLOAD_FILE_END:
      if (s_error.length()) return;
      if (!s_fwid_found) {
        Update.abort();
        s_error = "not a govee-dash image (identity marker missing)";
        Serial.println("[ota] rejected: identity marker not found");
        return;
      }
      if (Update.end(true)) {
        Serial.printf("[ota] OK: %u bytes flashed\n", (unsigned)up.totalSize);
      } else {
        s_error = Update.errorString();
        Serial.printf("[ota] end failed: %s\n", s_error.c_str());
      }
      break;

    case UPLOAD_FILE_ABORTED:
      Update.abort();
      s_error = "upload aborted by client";
      Serial.println("[ota] upload aborted");
      break;

    default:
      break;
  }
}

// POST /ota/upload — completion handler, called once the upload finishes
// (success or not). Reboots on success so the new image takes over.
static void handle_upload_done() {
  if (s_error.length()) {
    String e = s_error;
    e.replace("\"", "'");   // keep the JSON well-formed
    s_server->send(500, "application/json",
                   String("{\"ok\":false,\"error\":\"") + e + "\"}");
    return;
  }
  // Reply before rebooting so the browser can render the result.
  s_server->send(200, "application/json", "{\"ok\":true}");
  Serial.println("[ota] flashed; rebooting in 1.5 s");
  delay(1500);
  ESP.restart();
}

void ota_register(WebServer& server) {
  s_server = &server;
  server.on("/ota/upload", HTTP_POST, handle_upload_done, handle_upload_chunk);
}
