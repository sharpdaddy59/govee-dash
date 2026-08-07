// ui_grid.h — adaptive multi-sensor grid view.
//
// Shows every known Govee sensor at once, scaling the layout to the
// sensor count:
//   1 sensor  -> full-screen single view
//   2 sensors -> two full-width rows
//   3-4       -> 2x2 tiles
//   5-6       -> 3x2 tiles
//   7+        -> first 6 tiled, "+N" badge in the footer strip
//
// Each tile: alias/name, temperature (in the user's display unit),
// humidity, and a battery/RSSI footer. Fresh values render green, stale
// (unheard > BLE_STALE_AFTER_MS) render gray, low battery (<15%) yellow.
//
// Redraw strategy (inherited from hydro-dash's flicker lessons):
//   - full clear+redraw only on a "layout epoch" change (sensor set,
//     count, rotation, or a config change like alias/unit edits);
//   - otherwise per-tile snapshot diff — unchanged tiles are untouched,
//     changed tiles redraw values in place via setTextColor(fg, bg)
//     glyph-overwrite plus a trailing band clear for shrinking text.
// No sprites/canvases — the CYD has no PSRAM.

#pragma once

#include <Arduino.h>

void ui_grid_draw();
