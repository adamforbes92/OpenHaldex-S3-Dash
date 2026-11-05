// =====================================================================
// theme.h - colours, fonts and the layout model
//
// The palette is a direct port of dash.html (the browser reference), and so
// is the tile band. dash.html sized everything from one unit:
//
//     --u = min(viewport_w / 100, viewport_h / 56)
//
// At 800x480 that gives u = 8 px, which is where the tile padding comes from.
//
// The top band is no longer the ported tach ring: the ring, needle and hub
// cost ~58 ms a frame on this board and were replaced by a bar, which is a
// redesign rather than a port (see docs/redesign-bars.md and the gauge
// section below).
// =====================================================================
#pragma once

#include <lvgl.h>

#include "board_config.h"

// ------------------------------------------------------------- palette
// Same hex values as :root in dash.html.
#define COL_BG lv_color_hex(0x000000)
#define COL_PANEL lv_color_hex(0x0b0d10)
#define COL_LINE lv_color_hex(0x1c2128)
#define COL_FG lv_color_hex(0xe8edf2)
#define COL_DIM lv_color_hex(0x7c8894)
#define COL_GREEN lv_color_hex(0x22c55e)
#define COL_AMBER lv_color_hex(0xf59e0b)
#define COL_RED lv_color_hex(0xef4444)
#define COL_CYAN lv_color_hex(0x22d3ee)
#define COL_TRACK lv_color_hex(0x14181d)  // unfilled bar background
#define COL_LAMP_OFF lv_color_hex(0x0e1114)
#define COL_LAMP_EDGE lv_color_hex(0x2a2f36)
#define COL_LAMP_TXT_OFF lv_color_hex(0x3c444d)
#define COL_ZONE_GREEN lv_color_hex(0x123a22)  // unlit zone tints
#define COL_ZONE_AMBER lv_color_hex(0x3a2a10)
#define COL_ZONE_RED lv_color_hex(0x3a1414)
#define COL_WHITE lv_color_hex(0xf2f5f8)  // the big rev digits, boost tile bar

// --------------------------------------------------------------- fonts
// Montserrat sizes enabled in lv_conf.h.
#define FONT_CAP (&lv_font_montserrat_12)     // tile captions
#define FONT_SMALL (&lv_font_montserrat_14)   // peaks, sub lines, lamps
#define FONT_BODY (&lv_font_montserrat_16)    // status bar, lock rows
#define FONT_TEMP (&lv_font_montserrat_22)    // coolant / oil values
#define FONT_MODE (&lv_font_montserrat_28)    // Haldex mode name
#define FONT_BIG (&lv_font_montserrat_48)     // speed value
#define FONT_TICK (&lv_font_montserrat_14)    // numbers under the rev bar

// The two big numbers come from Montserrat Bold. LVGL ships Montserrat only
// up to 48 px, so they are generated from the TTF into src/ui/fonts by
// tools/make_fonts.sh. Only the glyphs they need are in there (digits, '-',
// '.'), which is why 96 px costs 43 KB rather than the ~150 KB a full size
// would. '-' is what draws "--" for a value that has never arrived.
LV_FONT_DECLARE(lv_font_dash_rpm_96);
LV_FONT_DECLARE(lv_font_dash_num_56);
#define FONT_RPM (&lv_font_dash_rpm_96)       // the big rev readout
#define FONT_NUM (&lv_font_dash_num_56)       // the boost tile's value

// ------------------------------------------------------------- layout
// All in pixels, evaluated once for the 800x480 panel.
namespace layout {

// --u = min(w/100, h/56) = min(8.0, 8.571) = 8
constexpr int U = (LCD_H_RES / 100 < LCD_V_RES / 56) ? (LCD_H_RES / 100) : (LCD_V_RES / 56);

constexpr int W = LCD_H_RES;
constexpr int H = LCD_V_RES;

// #status is flex 0 0 auto at height 2.8u, and #root is a column of
// [#top 56%, #bottom 44%, #status]. So the status bar is taken off first.
// dash.html gives 2.8u = 22 px here; that is a 22 px touch target on a
// 7" panel, so the bar is 30 px instead and the band above it absorbs the
// difference. Everything below is derived, so nothing else needs editing.
constexpr int STATUS_H = 30;                          // was 2.8u = 22
constexpr int CONTENT_H = H - STATUS_H;               // 450

// The band under the rev bar: the four original tiles, at the widths
// dash.html gives them (186 / 186 / 204 / 224 = 800). BOOST is one of them
// again - only REVS is full width.
constexpr int TILE_Y = 230;
constexpr int TILE_H = CONTENT_H - TILE_Y;            // 220

constexpr int COL0_X = 0;                             // SPEED
constexpr int COL0_W = (W * 186) / 800;               // 186
constexpr int COL1_X = COL0_X + COL0_W;               // 186
constexpr int COL1_W = COL0_W;                        // 186  BOOST
constexpr int COL2_X = COL1_X + COL1_W;               // 372
constexpr int COL2_W = (W * 204) / 800;               // 204  TEMPERATURES
constexpr int COL3_X = COL2_X + COL2_W;               // 576
constexpr int COL3_W = W - COL3_X;                    // 224  HALDEX

// .tile padding: 0.9u vertical, 1.1u horizontal
constexpr int TILE_PAD_V = (U * 9) / 10;  // 7
constexpr int TILE_PAD_H = (U * 11) / 10; // 8

// ---------------------------------------------------------------- gauge
// One bar across the whole panel, with the number it belongs to on a row of
// its own underneath. It replaces the old tach ring, needle and 9 ticks: a
// bar's moving edge is an opaque rectangle, so a change repaints only the
// cells that crossed the edge instead of a 279 px anti-aliased arc.
//
// The bar is BAR_SEG opaque cells with a BAR_GAP between them. 40 cells over
// the full 800 px is 20 px per cell, which still reads as a continuous bar at
// arm's length and keeps the per-frame repaint to about one cell.
constexpr int BAR_X = 0;
constexpr int BAR_W = W;                  // 0 .. 800: the only full-width item
constexpr int BAR_SEG = 40;
constexpr int BAR_GAP = 2;

constexpr int REV_BAR_Y = 6;
constexpr int REV_BAR_H = 104;            // fat: it is the one the eye tracks
constexpr int TICK_ROW_Y = 114;           // numbers under the rev bar
constexpr int TICK_ROW_H = 18;

// The big number gets its own row, centred, in the 96 px Montserrat Bold the
// bar's scale is read against. Its label is line-height tall, so the glyphs
// stay centred if the font size ever changes.
constexpr int RPM_NUM_Y = 132;
constexpr int RPM_NUM_H = TILE_Y - RPM_NUM_Y;  // 98

// ---------------------------------------------------------- tile band
// The tile band is the one part of the dashboard that changes shape with the
// car. Two geometries exist, both approved as mock-ups:
//
//   Full     SPEED | BOOST | TEMPERATURES | HALDEX, the widths dash.html
//            gives them (186 / 186 / 204 / 224) - docs/mockups/B
//   NoBoost  SPEED | TEMPERATURES | HALDEX, three equal panels - mock-up A,
//            for a car with no boost source to put in the fourth
//
// Which one is used is decided once at boot from what the car can actually
// feed (app/capability.h) and is never changed while the car is moving.
enum class Tile : uint8_t { Speed, Boost, Temps, Haldex };

struct TileCol {
  Tile role;
  int x, w;
};

struct TileBand {
  const TileCol *cols;
  int count;
  const char *name;
};

constexpr TileCol kTilesFull[] = {
    {Tile::Speed, COL0_X, COL0_W},
    {Tile::Boost, COL1_X, COL1_W},
    {Tile::Temps, COL2_X, COL2_W},
    {Tile::Haldex, COL3_X, COL3_W},
};

// Three equal panels: 266 / 266 / 268, so the third takes the rounding
// remainder and the band still ends exactly at the panel edge.
constexpr int NOBOOST_W = W / 3;
constexpr TileCol kTilesNoBoost[] = {
    {Tile::Speed, 0, NOBOOST_W},
    {Tile::Temps, NOBOOST_W, NOBOOST_W},
    {Tile::Haldex, 2 * NOBOOST_W, W - 2 * NOBOOST_W},
};

enum class Variant : uint8_t { Full, NoBoost };

constexpr TileBand kBandFull{kTilesFull, 4, "Full (B)"};
constexpr TileBand kBandNoBoost{kTilesNoBoost, 3, "NoBoost (A)"};

inline const TileBand &bandFor(Variant v) {
  return v == Variant::NoBoost ? kBandNoBoost : kBandFull;
}

}  // namespace layout
