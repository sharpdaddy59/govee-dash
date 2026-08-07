#include "ui_grid.h"
#include "ui.h"
#include "state.h"
#include "config.h"
#include "prefs.h"
#include "device_id.h"
#include "govee_decode.h"
#include <cmath>

// Bottom strip: this dashboard's hostname + a "+N" badge when more
// sensors are known than fit on screen.
static constexpr int STRIP_H  = 12;
static constexpr int SEP_GRAY = 0x4208;   // separator / chrome gray
// uint16_t (not uint32_t) for every color constant below — LovyanGFX's
// color path dispatches uint32_t to its RGB888 overload and the bytes
// get reinterpreted (TFT_GREEN ends up red).
static constexpr uint16_t COL_FRESH = TFT_GREEN;
static constexpr uint16_t COL_STALE = 0x7BEF;    // dim gray
static constexpr uint16_t COL_NAME  = TFT_WHITE;
static constexpr uint16_t COL_FOOT  = TFT_DARKGREY;
static constexpr uint16_t COL_LOWBAT = TFT_YELLOW;

// ---------------------------------------------------------------------------
// Layout tables. LovyanGFX default font is 6x8 px per text-size unit.
// ---------------------------------------------------------------------------

struct TileFonts { uint8_t name, temp, hum, foot; };

static void grid_dims(uint8_t n, uint8_t& cols, uint8_t& rows) {
  if (n <= 1)      { cols = 1; rows = 1; }
  else if (n == 2) { cols = 1; rows = 2; }
  else if (n <= 4) { cols = 2; rows = 2; }
  else             { cols = 3; rows = 2; }
}

static TileFonts fonts_for(uint8_t n) {
  if (n <= 1) return {3, 6, 4, 2};
  if (n == 2) return {2, 4, 3, 1};
  if (n <= 4) return {2, 3, 2, 1};
  return {1, 2, 2, 1};
}

// ---------------------------------------------------------------------------
// Per-tile snapshot for the in-place diff. Slots/ordering live in the
// layout epoch; this only carries the values drawn inside a tile.
// ---------------------------------------------------------------------------

struct TileSnap {
  float temp_c;
  float humidity;
  int   batt;
  int   rssi;
  bool  stale;
};

static bool feq(float a, float b) {
  if (isnan(a) && isnan(b)) return true;
  return a == b;
}

static bool snap_equal(const TileSnap& a, const TileSnap& b) {
  return feq(a.temp_c, b.temp_c) && feq(a.humidity, b.humidity) &&
         a.batt == b.batt && a.rssi == b.rssi && a.stale == b.stale;
}

// Layout-epoch state — sentinels force a full first-frame draw.
static int8_t   s_last_slots[GRID_MAX_TILES];
static uint8_t  s_last_shown   = 255;
static uint8_t  s_last_total   = 255;
static uint32_t s_last_cfg_gen = (uint32_t)-1;
static TileSnap s_snap[GRID_MAX_TILES];

static float display_temp(float c) {
  if (isnan(c)) return c;
  return (prefs_temp_unit() == TEMP_UNIT_FAHRENHEIT) ? c * 9.0f / 5.0f + 32.0f
                                                     : c;
}

// Print `text` at (x, y) in the given size/color, then clear the
// remainder of the row band out to `right` so a value that shrank in
// width doesn't leave stale glyph pixels behind. setTextColor(fg, bg)
// makes each glyph overwrite its own cell — no clear-then-draw flash.
static void print_row(int x, int y, int right, uint8_t size,
                      uint16_t color, const char* text) {
  auto& g = ui_gfx();
  g.setTextSize(size);
  g.setTextColor(color, (uint16_t)TFT_BLACK);
  g.setCursor(x, y);
  g.print(text);
  int cx = g.getCursorX();
  int h  = size * 8;
  if (cx < right) g.fillRect(cx, y, right - cx, h, (uint16_t)TFT_BLACK);
}

// Draw one tile. `full` = the tile background is known-black (layout
// epoch redraw); otherwise values overwrite in place and the name row
// (invariant between epochs) is skipped.
static void draw_tile(int tx, int ty, int tw, int th, const DeviceRecord& d,
                      const TileFonts& f, bool full, bool single) {
  auto& g = ui_gfx();

  // Clip to the tile so long names/values can never bleed into a
  // neighbouring tile. Leave 2 px before the right edge clear of the
  // separator line.
  int pad = single ? 10 : 4;
  g.setClipRect(tx, ty, tw, th);
  g.setTextWrap(false);

  int right = tx + tw - 2;
  int name_h = f.name * 8, temp_h = f.temp * 8, hum_h = f.hum * 8,
      foot_h = f.foot * 8;
  int avail = th - 2 * pad - foot_h;
  int gap   = (avail - (name_h + temp_h + hum_h)) / 3;
  if (gap < 1) gap = 1;
  int name_y = ty + pad;
  int temp_y = name_y + name_h + gap;
  int hum_y  = temp_y + temp_h + gap;
  int foot_y = ty + th - pad - foot_h;

  bool stale = d.stale.load();
  uint16_t vcol = stale ? COL_STALE : COL_FRESH;

  // Name — alias falls back to the synthesized hostname. Only drawn on
  // an epoch redraw: it's invariant in between (alias edits bump
  // g_ui_config_gen, which forces an epoch redraw).
  if (full) {
    const char* alias = prefs_alias_for(d.hostname);
    print_row(tx + pad, name_y, right, f.name,
              stale ? COL_STALE : COL_NAME, *alias ? alias : d.hostname);
  }

  char buf[24];
  float t = display_temp(d.temp_c.load());
  if (isnan(t)) snprintf(buf, sizeof(buf), "--");
  else snprintf(buf, sizeof(buf), "%.1f%c", t,
                (prefs_temp_unit() == TEMP_UNIT_FAHRENHEIT) ? 'F' : 'C');
  print_row(tx + pad, temp_y, right, f.temp, vcol, buf);

  float h = d.humidity.load();
  if (isnan(h)) snprintf(buf, sizeof(buf), "--");
  else          snprintf(buf, sizeof(buf), "%.1f%%", h);
  print_row(tx + pad, hum_y, right, f.hum, vcol, buf);

  // Footer: battery then RSSI. Battery goes yellow below 15% (unless
  // the whole tile is stale-gray). Two prints so only the battery part
  // changes color; the trailing band clear runs after the second.
  int bp = d.battery_pct.load();
  uint16_t bcol = stale ? COL_STALE
                : (bp >= 0 && bp < 15) ? COL_LOWBAT : COL_FOOT;
  g.setTextSize(f.foot);
  g.setTextColor(bcol, (uint16_t)TFT_BLACK);
  g.setCursor(tx + pad, foot_y);
  if (bp >= 0) snprintf(buf, sizeof(buf), "%d%% ", bp);
  else         snprintf(buf, sizeof(buf), "--%% ");
  g.print(buf);
  g.setTextColor(stale ? COL_STALE : COL_FOOT, (uint16_t)TFT_BLACK);
  snprintf(buf, sizeof(buf), "%ddBm%s", d.rssi.load(),
           stale ? " gone" : "");
  g.print(buf);
  int cx = g.getCursorX();
  if (cx < right) g.fillRect(cx, foot_y, right - cx, foot_h,
                             (uint16_t)TFT_BLACK);

  g.setTextWrap(true);
  g.clearClipRect();
}

static void draw_footer_strip(uint8_t shown, uint8_t total) {
  auto& g = ui_gfx();
  int W = g.width(), H = g.height();
  int y = H - STRIP_H;
  g.fillRect(0, y, W, STRIP_H, (uint16_t)TFT_BLACK);
  g.drawFastHLine(0, y, W, (uint16_t)SEP_GRAY);
  g.setTextSize(1);
  g.setTextColor(COL_FOOT, (uint16_t)TFT_BLACK);
  g.setCursor(4, y + 3);
  g.print(device_hostname());
  if (total > shown) {
    char buf[16];
    snprintf(buf, sizeof(buf), "+%d more", total - shown);
    int tw = g.textWidth(buf);
    g.setCursor(W - tw - 4, y + 3);
    g.print(buf);
  }
}

void ui_grid_draw() {
  auto& g = ui_gfx();
  int W = g.width(), H = g.height();
  int grid_h = H - STRIP_H;

  // Collect the sensors to tile, in slot order (stable — slots never
  // move thanks to the tombstone table).
  int8_t  slots[GRID_MAX_TILES];
  uint8_t shown = 0, total = 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!g_devices[i].valid.load()) continue;
    total++;
    if (shown < GRID_MAX_TILES) slots[shown++] = (int8_t)i;
  }

  // Layout epoch: the shown-slot set, the total count (for the +N
  // badge), and the config generation (alias / temp-unit edits). Any
  // change -> full clear + redraw; covers insert, forget, and expiry.
  uint32_t cfg_gen = g_ui_config_gen.load(std::memory_order_relaxed);
  bool full = (shown != s_last_shown) || (total != s_last_total) ||
              (cfg_gen != s_last_cfg_gen);
  if (!full) {
    for (uint8_t i = 0; i < shown; i++) {
      if (slots[i] != s_last_slots[i]) { full = true; break; }
    }
  }
  if (full) {
    s_last_shown   = shown;
    s_last_total   = total;
    s_last_cfg_gen = cfg_gen;
    for (uint8_t i = 0; i < shown; i++) s_last_slots[i] = slots[i];
    g.fillScreen(TFT_BLACK);
    draw_footer_strip(shown, total);
  }

  if (shown == 0) {
    if (full) {
      const char* msg = "scanning for Govee sensors...";
      g.setTextSize(2);
      g.setTextColor(TFT_DARKGREY, (uint16_t)TFT_BLACK);
      int tw = g.textWidth(msg);
      g.setCursor((W - tw) / 2, grid_h / 2 - 8);
      g.print(msg);
    }
    return;
  }

  uint8_t cols, rows;
  grid_dims(shown, cols, rows);
  TileFonts f = fonts_for(shown);
  int tw = W / cols;
  int th = grid_h / rows;

  if (full) {
    // Tile separators — thin gray lines between columns and rows.
    for (uint8_t c = 1; c < cols; c++)
      g.drawFastVLine(c * tw, 0, grid_h, (uint16_t)SEP_GRAY);
    for (uint8_t r = 1; r < rows; r++)
      g.drawFastHLine(0, r * th, W, (uint16_t)SEP_GRAY);
  }

  for (uint8_t i = 0; i < shown; i++) {
    const DeviceRecord& d = g_devices[slots[i]];
    TileSnap now{ d.temp_c.load(), d.humidity.load(),
                  d.battery_pct.load(), d.rssi.load(), d.stale.load() };
    if (!full && snap_equal(now, s_snap[i])) continue;   // untouched tile
    s_snap[i] = now;

    uint8_t col = i % cols, row = i / cols;
    int x = col * tw, y = row * th;
    // Last column/row absorb the integer-division remainder.
    int w = (col == cols - 1) ? (W - x) : tw;
    int h = (row == rows - 1) ? (grid_h - y) : th;
    // Inset past the separator lines so tiles never draw over them.
    if (col > 0) { x += 1; w -= 1; }
    if (row > 0) { y += 1; h -= 1; }

    draw_tile(x, y, w, h, d, f, full, shown == 1);
  }
}
