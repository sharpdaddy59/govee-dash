#include "device_id.h"
#include <Arduino.h>

const char* device_hostname() {
  static char buf[20] = {0};
  if (buf[0] == 0) {
    // ESP.getEfuseMac() reads the base MAC straight from chip eFuse
    // and works before WiFi is initialized. WiFi.macAddress() returns
    // zeros when called before the radio has been put into any mode.
    //
    // Byte order in the returned uint64_t: bits 0..7 = MAC byte 5
    // (LSB of the canonical "AA:BB:CC:DD:EE:FF" display order),
    // bits 8..15 = byte 4, etc. So the last 4 hex chars of the MAC
    // are byte 4 followed by byte 5.
    uint64_t mac = ESP.getEfuseMac();
    uint8_t byte4 = (mac >> 8) & 0xFF;
    uint8_t byte5 = (mac >> 0) & 0xFF;
    snprintf(buf, sizeof(buf), "govee-dash-%02x%02x", byte4, byte5);
  }
  return buf;
}
