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

static WebServer* s_server = nullptr;
static String     s_error;            // empty = no error so far

// POST /ota/upload — chunk handler, called repeatedly during the upload.
static void handle_upload_chunk() {
  HTTPUpload& up = s_server->upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      s_error = "";
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
      if (Update.write(up.buf, up.currentSize) != up.currentSize) {
        s_error = Update.errorString();
        Serial.printf("[ota] write failed: %s\n", s_error.c_str());
      }
      break;

    case UPLOAD_FILE_END:
      if (s_error.length()) return;
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
