# Govee sensors — BLE advertisement formats

govee-dash reads Govee temperature/humidity sensors by passively
listening to their Bluetooth Low Energy advertisements — no pairing, no
connection. This documents the wire formats and how the firmware decodes
them. Decode logic lives in [`govee_decode.cpp`](../govee_decode.cpp);
the scan loop in [`ble_scanner.cpp`](../ble_scanner.cpp); scan tunables
in [`config.h`](../config.h).

## Why passive scanning

Supported Govee models broadcast their current reading in the
manufacturer-specific data of their BLE advertisement, roughly every
2 seconds. govee-dash runs a **passive scan** — it never transmits scan
requests and never connects or pairs. It just listens.

- Every sensor in range is picked up simultaneously; each becomes its
  own dashboard tile.
- A passive scan is cheap on the WROOM-32's shared 2.4 GHz radio. The
  `BLE_SCAN_*` constants in `config.h` gap the scan (window < interval,
  pause between windows) so BLE listening does not starve WiFi.
- Trade-off: a passive scan does **not** receive scan-response packets.
  If a sensor puts its name only in the scan response, that name is
  invisible — so the firmware filters on the manufacturer-data
  **company ID + payload length**, not the device name. The name is
  only an opportunistic secondary check when one happens to be present.

## Dispatch

The decoder table in `govee_decode.cpp` keys on
**(company ID, exact payload length)** — the company ID is parsed
little-endian from the first two manufacturer-data bytes
(`md[0] | md[1] << 8`), and the payload length is counted **after**
stripping those two bytes. Length is load-bearing: the H5074 and H5075
share company ID `0xEC88` and differ only in length.

| Model | On-air CID bytes | Parsed CID | Payload len | Verified? |
|-------|------------------|------------|-------------|-----------|
| H5075 | `88 EC` | `0xEC88` | 6 | **yes** (hardware, May 2026, via hydro-dash) |
| H5074 | `88 EC` | `0xEC88` | 7 | no — literature-derived |
| H5102 | `01 00` | `0x0001` | 6 | no — literature-derived |
| H5179 | `01 88` | `0x8801` | 9 | no — literature-derived |

Sources for the unverified layouts: Home Assistant's `govee-ble`
library (`govee_ble/parser.py`) and the Theengs decoder. **Verify each
against a real device before trusting it** — see
[Verifying a model](#verifying-a-model) below.

## Per-model layouts

Offsets below are into the payload with the 2-byte company ID already
stripped (`p[0]` = third byte on air).

### H5075 — packed 24-bit (verified)

```
p:      0     1  2  3     4     5
        pad   ──packed──  batt  trail
```

```
packed   = (p[1] << 16) | (p[2] << 8) | p[3]     // big-endian
negative = packed & 0x800000                     // top bit = temp sign
mag      = packed & 0x7FFFFF
temp_c   = (negative ? -1 : +1) * mag / 10000.0
humidity = (mag % 1000) / 10.0
battery  = p[4]
```

Worked example — manufacturer data `88 EC 00 03 6A 4C 5A 00`:
`packed = 0x036A4C = 224332` → temp **22.43 °C**, humidity
`(224332 mod 1000)/10` = **33.2 %**, battery `0x5A` = **90 %**.

### H5074 — little-endian int16 (unverified)

```
p:      0     1  2      3  4      5     6
        pad   temp LE   hum LE    batt  trail (often 0x02)
```

```
temp_c   = int16(p[1] | p[2] << 8) / 100.0       // negatives native
humidity = uint16(p[3] | p[4] << 8) / 100.0
battery  = p[5]
```

### H5102 — packed 24-bit, shifted (unverified)

Same packed math as the H5075 but one byte later:

```
p:      0     1        2  3  4     5
        pad   subtype  ──packed──  batt
```

**Caution:** company ID `0x0001` is Nordic Semiconductor's SIG-assigned
ID and appears in many unrelated devices' manufacturer data. The exact
6-byte length match, the sanity gate, and the name-if-present guard
keep false positives out; a residual ghost tile can be removed with
Forget + "also ignore" in the web console.

### H5179 — header + little-endian int16 (unverified)

```
p:      0  1  2  3     4  5      6  7      8
        ──header────   temp LE   hum LE    batt
        (often EC 00 01 01)
```

## Sanity gate

Every decoder rejects an implausible result — temperature outside
−40…80 °C, humidity outside 0…100 %, or battery outside 0…100 — and the
advertisement is dropped. So a wrong company ID, length, or byte offset
**fails safe** (no tile / no update) instead of painting garbage.

## Verifying a model

1. Set `BLE_DEBUG` to `1` at the top of `ble_scanner.cpp`, build, flash,
   open the serial monitor (`.\build.ps1 -Upload -Monitor`).
2. Every decoded advert logs its model, MAC, raw hex, and decoded
   values. Compare temperature/humidity against the sensor's own LCD
   (expect agreement within ±0.3 °C / ±1 %).
3. Watch for `[ble] ??` lines — the safety net that logs any advert
   named `GV*` that the dispatch did NOT match (unknown company ID, or
   a known ID with an unexpected length). That's how a wrong table
   entry surfaces instead of silently producing zero results.
4. Test the negative-temperature path (sensor in a freezer for a few
   minutes): packed-sign handling for H5075/H5102, native int16 for
   H5074/H5179.
5. Update the "Verified?" column above when a model passes.

## How govee-dash uses it

- Each sensor becomes a `DeviceRecord` keyed by a synthesized hostname
  `govee-<last 3 MAC octets>` (stable across reboots — these sensors
  use a fixed public address).
- The BLE scan task owns staleness: a sensor unheard for
  `BLE_STALE_AFTER_MS` (default 90 s) grays out. If the auto-forget
  pref is set, a sensor unheard past that many hours is removed from
  the table entirely (and re-inserts if it returns).
- Ignored MACs (web console → Forget + "also ignore") are dropped
  before insertion.
- Library: NimBLE-Arduino 2.x (scan-only, pinned in setup.ps1).

## Adding another model

1. Write a decoder in `govee_decode.cpp` following the existing four.
2. Add a `GoveeModelSpec` row — company ID (as parsed LE), exact
   payload length, model enum, decoder pointer — and extend the
   `GoveeModel` enum + `govee_model_label`.
3. If the payload length collides with an existing entry on the same
   company ID, add a per-model secondary check instead of widening the
   match.
4. Verify with `BLE_DEBUG` as above; update this document.

The `DeviceRecord` + `state_upsert` path is model-agnostic — only the
decode step differs.
