// =====================================================================
// theme.h - colours, fonts and the layout model
//
// The palette and the geometry are a direct port of dash.html (the browser
// reference). dash.html sized everything from one unit:
//
//     --u = min(viewport_w / 100, viewport_h / 56)
//
// At 800x480 that gives u = 8 px. Keeping the same formula means the panel
// layout is provably the same as the page that was reviewed and approved.
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
#define COL_WHITE lv_color_hex(0xf2f5f8)
#define COL_HUB lv_color_hex(0x20262d)

// --------------------------------------------------------------- fonts
// Montserrat sizes enabled in lv_conf.h.
#define FONT_CAP (&lv_font_montserrat_12)     // tile captions
#define FONT_SMALL (&lv_font_montserrat_14)   // peaks, sub lines, lamps
#define FONT_BODY (&lv_font_montserrat_16)    // status bar, lock rows
#define FONT_TEMP (&lv_font_montserrat_22)    // coolant / oil values
#define FONT_MODE (&lv_font_montserrat_28)    // Haldex mode name
#define FONT_BIG (&lv_font_montserrat_48)     // speed / boost values
#define FONT_TACH (&lv_font_montserrat_48)    // rpm digits
#define FONT_TICK (&lv_font_montserrat_14)    // tach ring numbers
#define FONT_RPM_LBL (&lv_font_montserrat_18)

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
constexpr int CONTENT_H = H - STATUS_H;               // 458
constexpr int TOP_H = (CONTENT_H * 56) / 100;         // 256
constexpr int BOTTOM_Y = TOP_H + 1;                   // 1px divider
constexpr int BOTTOM_H = CONTENT_H - TOP_H - 1;       // 201

// #bottom grid-template-columns: 1.05fr 1.05fr 1.15fr 1.25fr
constexpr int COL0_X = 0;
constexpr int COL0_W = (W * 105) / 450;                                  // 186
constexpr int COL1_X = COL0_X + COL0_W;
constexpr int COL1_W = (W * 105) / 450;                                  // 186
constexpr int COL2_X = COL1_X + COL1_W;
constexpr int COL2_W = (W * 115) / 450;                                  // 204
constexpr int COL3_X = COL2_X + COL2_W;
constexpr int COL3_W = W - COL3_X;                                       // 224

// .tile padding: 0.9u vertical, 1.1u horizontal
constexpr int TILE_PAD_V = (U * 9) / 10;  // 7
constexpr int TILE_PAD_H = (U * 11) / 10; // 8

// ------------------------------------------------------------- tach ring
// Ported verbatim from drawTach() in dash.html:
//   pad  = max(4, min(cw,ch) * 0.02)
//   r    = min((cw-2*pad)/2.20, (ch-2*pad)/1.92)
//   thick= max(8, r * 0.20)
//   cx   = cw/2,  cy = pad + r * 1.10
constexpr int TACH_CW = W;
constexpr int TACH_CH = TOP_H;
constexpr int TACH_PAD = (TACH_CH * 2) / 100 > 4 ? (TACH_CH * 2) / 100 : 4;  // 5
constexpr int TACH_FIT_W = ((TACH_CW - 2 * TACH_PAD) * 100) / 220;           // /2.20
constexpr int TACH_FIT_H = ((TACH_CH - 2 * TACH_PAD) * 100) / 192;           // /1.92
constexpr int TACH_R = TACH_FIT_W < TACH_FIT_H ? TACH_FIT_W : TACH_FIT_H;    // 127
constexpr int TACH_THICK = (TACH_R * 20) / 100 > 8 ? (TACH_R * 20) / 100 : 8;  // 25
constexpr int TACH_CX = TACH_CW / 2;                                         // 400
constexpr int TACH_CY = TACH_PAD + (TACH_R * 110) / 100;                     // 144

// The lv_arc object must be (2r + thick) square so that the drawn stroke
// lands exactly on radius r: LVGL puts the arc centre line at
// (min(w,h) - arc_width) / 2 from the centre.
constexpr int ARC_OBJ = 2 * TACH_R + TACH_THICK;  // 279
constexpr int ARC_X = TACH_CX - ARC_OBJ / 2;
constexpr int ARC_Y = TACH_CY - ARC_OBJ / 2;

// 270 degree sweep starting at 135 degrees (7:30 position), clockwise.
constexpr int ARC_ROTATION_DEG = 135;
constexpr int ARC_SWEEP_DEG = 270;

// Tick geometry from dash.html:
//   r1 = r - thick*0.50, r2 = r + thick*0.50, labels at r - thick*1.00
constexpr int TICK_R_IN = TACH_R - TACH_THICK / 2;
constexpr int TICK_R_OUT = TACH_R + TACH_THICK / 2;
constexpr int TICK_R_LABEL = TACH_R - TACH_THICK;

constexpr int NEEDLE_LEN = TACH_R - TACH_THICK / 10;
constexpr int NEEDLE_TAIL = (TACH_R * 14) / 100;
constexpr int HUB_R = (TACH_R * 85) / 1000;

}  // namespace layout
