// govee_decode.cpp — per-model Govee decoders + dispatch table.
//
// Payload offsets below are into the manufacturer data with the 2-byte
// company ID already stripped (matching hydro-dash's decode_h5075
// convention). Full byte-layout tables in docs/govee-ble.md.
//
// VERIFICATION STATUS:
//   H5075 — verified against hardware (inherited from hydro-dash).
//   H5074, H5102, H5179 — hypotheses from HA govee-ble / Theengs; set
//   BLE_DEBUG 1 in ble_scanner.cpp and compare against each sensor's own
//   LCD before trusting. Include a freezer test for the negative-temp
//   paths (packed-sign for H5075/H5102, native int16 for H5074/H5179).

#include "govee_decode.h"
#include <cmath>

// Shared plausibility gate. Rejecting nonsense means a wrong company ID
// or wrong byte offsets fail SAFE — the advert is dropped, not displayed.
static bool sane(GoveeReading& r) {
  if (r.temp_c   < -40.0f || r.temp_c   > 80.0f)  return false;
  if (r.humidity <   0.0f || r.humidity > 100.0f) return false;
  if (r.battery_pct < 0   || r.battery_pct > 100) return false;
  r.ok = true;
  return true;
}

// H5075 — 6-byte payload. p[0] pad; p[1..3] 24-bit BIG-endian packed
// value, top bit = temperature sign; p[4] battery; p[5] trailing.
// temp_c = ±mag/10000, humidity = (mag % 1000) / 10.
static GoveeReading decode_h5075(const uint8_t* p, size_t len) {
  GoveeReading r{NAN, NAN, -1, false};
  if (len < 5) return r;

  uint32_t packed   = ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
  bool     negative = (packed & 0x800000u) != 0;
  uint32_t mag      = packed & 0x7FFFFFu;

  r.temp_c      = (negative ? -1.0f : 1.0f) * (mag / 10000.0f);
  r.humidity    = (mag % 1000) / 10.0f;
  r.battery_pct = p[4];
  sane(r);
  return r;
}

// H5074 — 7-byte payload (same 0xEC88 company ID as the H5075; only the
// length separates them). p[0] pad (0x00); p[1..2] temp int16
// LITTLE-endian, /100 °C (negatives are native two's complement);
// p[3..4] humidity uint16 LE, /100 %; p[5] battery; p[6] trailing
// (commonly 0x02).
static GoveeReading decode_h5074(const uint8_t* p, size_t len) {
  GoveeReading r{NAN, NAN, -1, false};
  if (len < 6) return r;

  int16_t  traw = (int16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
  uint16_t hraw = (uint16_t)p[3] | ((uint16_t)p[4] << 8);

  r.temp_c      = traw / 100.0f;
  r.humidity    = hraw / 100.0f;
  r.battery_pct = p[5];
  sane(r);
  return r;
}

// H5102 — 6-byte payload under company ID 0x0001. Same packed 24-bit
// big-endian math as the H5075 but shifted one byte later: p[0] pad;
// p[1] subtype (often 0x01); p[2..4] packed; p[5] battery.
//
// RISK: 0x0001 is Nordic Semiconductor's SIG-assigned company ID and
// appears in many unrelated devices' manufacturer data. Mitigation
// ladder: exact 6-byte length match, this sanity gate, and the
// name-if-present reject guard in ble_scanner.cpp. Residual ghosts are
// covered by forget+ignore in the web UI.
static GoveeReading decode_h5102(const uint8_t* p, size_t len) {
  GoveeReading r{NAN, NAN, -1, false};
  if (len < 6) return r;

  uint32_t packed   = ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 8) | p[4];
  bool     negative = (packed & 0x800000u) != 0;
  uint32_t mag      = packed & 0x7FFFFFu;

  r.temp_c      = (negative ? -1.0f : 1.0f) * (mag / 10000.0f);
  r.humidity    = (mag % 1000) / 10.0f;
  r.battery_pct = p[5];
  sane(r);
  return r;
}

// H5179 — 9-byte payload under company ID 0x8801. p[0..3] header
// (commonly EC 00 01 01); p[4..5] temp int16 LE /100; p[6..7] humidity
// uint16 LE /100; p[8] battery.
static GoveeReading decode_h5179(const uint8_t* p, size_t len) {
  GoveeReading r{NAN, NAN, -1, false};
  if (len < 9) return r;

  int16_t  traw = (int16_t)((uint16_t)p[4] | ((uint16_t)p[5] << 8));
  uint16_t hraw = (uint16_t)p[6] | ((uint16_t)p[7] << 8);

  r.temp_c      = traw / 100.0f;
  r.humidity    = hraw / 100.0f;
  r.battery_pct = p[8];
  sane(r);
  return r;
}

// ---------------------------------------------------------------------------
// Dispatch table. company_id is as parsed from the wire little-endian
// (md[0] | md[1]<<8) — so on-air bytes "88 EC" appear here as 0xEC88.
// payload_len is EXACT (post-CID-strip); if a Govee firmware revision
// changes a length, relax that entry to a min-length + secondary check
// rather than silently widening the match.
// ---------------------------------------------------------------------------

struct GoveeModelSpec {
  uint16_t   company_id;
  uint8_t    payload_len;
  GoveeModel model;
  GoveeReading (*decode)(const uint8_t* p, size_t len);
};

static const GoveeModelSpec GOVEE_MODELS[] = {
  { 0xEC88, 6, GoveeModel::H5075, decode_h5075 },
  { 0xEC88, 7, GoveeModel::H5074, decode_h5074 },
  { 0x0001, 6, GoveeModel::H5102, decode_h5102 },
  { 0x8801, 9, GoveeModel::H5179, decode_h5179 },
};
static const size_t GOVEE_MODEL_COUNT =
    sizeof(GOVEE_MODELS) / sizeof(GOVEE_MODELS[0]);

bool govee_cid_known(uint16_t cid) {
  for (size_t i = 0; i < GOVEE_MODEL_COUNT; i++) {
    if (GOVEE_MODELS[i].company_id == cid) return true;
  }
  return false;
}

GoveeReading govee_decode(uint16_t cid, const uint8_t* payload, size_t len,
                          GoveeModel* out_model) {
  if (out_model) *out_model = GoveeModel::UNKNOWN;
  for (size_t i = 0; i < GOVEE_MODEL_COUNT; i++) {
    const GoveeModelSpec& s = GOVEE_MODELS[i];
    if (s.company_id != cid || s.payload_len != len) continue;
    GoveeReading r = s.decode(payload, len);
    if (r.ok && out_model) *out_model = s.model;
    return r;
  }
  return GoveeReading{NAN, NAN, -1, false};
}

const char* govee_model_label(GoveeModel m) {
  switch (m) {
    case GoveeModel::H5075: return "H5075";
    case GoveeModel::H5074: return "H5074";
    case GoveeModel::H5102: return "H5102";
    case GoveeModel::H5179: return "H5179";
    default:                return "?";
  }
}
