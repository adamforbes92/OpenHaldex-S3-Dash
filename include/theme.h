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
// cost ~58 ms a frame on this board and were replaced by two bars, which is
// a redesign rather than a port (see docs/mockups and the gauge section
// below).
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
#define COL_WHITE lv_color_hex(0xf2f5f8)  // boost bar, big rev digits

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
#define FONT_NUM (&lv_font_dash_num_56)       // the boost value beside it

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
// 7" panel, so the bar is 30 px instead and the tach band absorbs the
// difference. Everything below is derived, so nothing else needs editing.
constexpr int STATUS_H = 30;                          // was 2.8u = 22
constexpr int CONTENT_H = H - STATUS_H;               // 450
constexpr int TOP_H = (CONTENT_H * 56) / 100;         // 252
constexpr int BOTTOM_Y = TOP_H + 1;                   // 1px divider
constexpr int BOTTOM_H = CONTENT_H - TOP_H - 1;       // 197

// #bottom is three tiles now: SPEED | TEMPERATURES | HALDEX. Boost moved to
// the top of the screen as a bar, so its tile and its column are gone.
constexpr int COL0_X = 0;
constexpr int COL0_W = (W * 3) / 10;                                     // 240  SPEED
constexpr int COL1_X = COL0_X + COL0_W;
constexpr int COL1_W = (W * 13) / 40;                                    // 260  TEMPERATURES
constexpr int COL2_X = COL1_X + COL1_W;
constexpr int COL2_W = W - COL2_X;                                       // 300  HALDEX

// .tile padding: 0.9u vertical, 1.1u horizontal
constexpr int TILE_PAD_V = (U * 9) / 10;  // 7
constexpr int TILE_PAD_H = (U * 11) / 10; // 8

// ---------------------------------------------------------------- gauge
// Two bars stacked on the left, the numbers they belong to in a column on
// the right. The bars replace the old tach ring, needle and 9 ticks: a bar's
// moving edge is an opaque rectangle, so a change repaints only the cells
// that crossed the edge instead of a 279 px anti-aliased arc.
//
// Each bar is BAR_SEG opaque cells with a BAR_GAP between them. 40 cells over
// 512 px is 12.8 px per cell, which still reads as a continuous bar at arm's
// length and keeps the per-frame repaint to about one cell.
constexpr int BAR_X = 8;
constexpr int BAR_W = 512;                // 8 .. 520
constexpr int BAR_SEG = 40;
constexpr int BAR_GAP = 2;

constexpr int BOOST_BAR_Y = 6;
constexpr int BOOST_BAR_H = 56;           // the fat one on top
constexpr int REV_BAR_Y = 70;
constexpr int REV_BAR_H = 142;            // fatter still, below it
constexpr int TICK_ROW_Y = 218;           // numbers under the rev bar
constexpr int TICK_ROW_H = 20;

// The value column: BOOST over REVS, each with a caption, the way the
// approved mock-up has it. Everything in it is right-aligned to VAL_R.
constexpr int VAL_DIV_X = 511;            // vertical rule between bars/values
constexpr int VAL_X = 520;
constexpr int VAL_R = W - 8;              // 792
constexpr int VAL_W = VAL_R - VAL_X;      // 272
constexpr int VAL_CAP_Y = 8;              // "BOOST" / "RPM" captions
constexpr int VAL_BOOST_Y = 22;           // 56 px value
constexpr int VAL_UNIT_Y = 78;            // "bar" / "psi" / "mbar"
constexpr int VAL_DIV_Y = 104;            // horizontal rule
constexpr int VAL_RPM_Y = 132;            // 96 px value

}  // namespace layout
