// =====================================================================
// dash_ui.cpp - the LVGL dashboard, a native port of dash.html
//
// Widget map (see theme.h for the numbers):
//
//   +--------------------------------------------------------------+
//   |  tach ring: 3 zone arcs, 9 ticks, needle, hub, rpm digits    |  top 252 px
//   +----------+----------+-------------+-------------------------+
//   |  SPEED   |  BOOST   | TEMPERATURES|  HALDEX                 |  bottom 197 px
//   |  123     |  1.25 bar| cool / oil  |  mode, lock req/act, thr |
//   +----------+----------+-------------+-------------------------+
//   | * LIVE  BRAKE HAND TC ASR AWD     peaks   RESET  DAY  SETTINGS| status 30 px
//   +--------------------------------------------------------------+
//
// Reading order matches the page: fast values come from /api/live, slow
// flags from /api/dashboard, and nothing is drawn for a value the C6
// reported as null ("--" instead of 0).
// =====================================================================
#include "ui/dash_ui.h"

#include <Arduino.h>
#include <cmath>
#include <cstring>

#include "hal/crowpanel.h"
#include "net/telemetry.h"
#include "app/config.h"
#include "theme.h"

namespace ui {
namespace {

namespace L = layout;

constexpr float kPi = 3.14159265358979f;
constexpr float kTau = 0.11f;  // interpolation time constant, as in dash.html
constexpr uint32_t kFlashMs = 60;

const char *const kModeNames[6] = {"STOCK", "FWD", "50:50", "60:40", "75:25", "EXPERT"};

// ------------------------------------------------------------- handles
struct Widgets {
  lv_obj_t *tach = nullptr;

  lv_obj_t *arcGreen = nullptr, *arcAmber = nullptr, *arcRed = nullptr;
  lv_obj_t *needle = nullptr, *hub = nullptr;
  lv_obj_t *rpmDigits = nullptr, *shiftLamp = nullptr;
  lv_obj_t *tickLine[9] = {};
  lv_obj_t *tickLabel[9] = {};

  lv_obj_t *speedVal = nullptr, *speedPeak = nullptr;
  lv_obj_t *boostVal = nullptr, *boostUnit = nullptr, *boostBar = nullptr, *boostPeak = nullptr;
  lv_obj_t *coolVal = nullptr, *coolBar = nullptr, *oilVal = nullptr, *oilBar = nullptr;
  lv_obj_t *modeVal = nullptr, *haldexSub = nullptr;
  lv_obj_t *lockReqVal = nullptr, *lockActVal = nullptr, *thrBar = nullptr;

  lv_obj_t *dot = nullptr, *connTxt = nullptr, *peakTxt = nullptr;
  lv_obj_t *lampBrake = nullptr, *lampHand = nullptr, *lampTc = nullptr, *lampAsr = nullptr,
           *lampAwd = nullptr;
  lv_obj_t *btnReset = nullptr, *btnDay = nullptr, *btnCfg = nullptr;
  lv_obj_t *dayLabel = nullptr;

  lv_obj_t *stale = nullptr, *staleBig = nullptr, *staleSmall = nullptr;
  lv_obj_t *settings = nullptr, *kb = nullptr;
} w;

struct Values {
  float rpm, speed, throttle, boost, coolant, oil, lockTarget, lockActual;
};
constexpr Values kBlank = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};

Values g_target = kBlank;  // latest from the API
Values g_shown = kBlank;   // interpolated, what is drawn
struct {
  float rpm = NAN, speed = NAN, boost = NAN;
} g_peaks;

bool g_flashOn = false;
uint32_t g_lastFlashMs = 0;
uint32_t g_lastTickMs = 0;
bool g_settingsOpen = false;
bool g_stale = true;

// Persistent storage for the needle's two points: LVGL keeps the pointer.
lv_point_precise_t g_needlePts[2] = {{0, 0}, {0, 0}};

// ------------------------------------------------------------- helpers
inline float angFor(float frac) {
  if (!(frac >= 0.0f)) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  return (L::ARC_ROTATION_DEG + frac * L::ARC_SWEEP_DEG) * kPi / 180.0f;
}

void setText(lv_obj_t *label, const char *text) {
  const char *cur = lv_label_get_text(label);
  if (!cur || strcmp(cur, text) != 0) lv_label_set_text(label, text);
}

void setTextColor(lv_obj_t *label, lv_color_t c) { lv_obj_set_style_text_color(label, c, 0); }

void setBar(lv_obj_t *bar, int32_t value) {
  if (lv_bar_get_value(bar) != value) lv_bar_set_value(bar, value, LV_ANIM_OFF);
}

lv_color_t colFor(float v, float warn, float crit) {
  if (isnan(v)) return COL_DIM;
  if (v >= crit) return COL_RED;
  if (v >= warn) return COL_AMBER;
  return COL_GREEN;
}

lv_obj_t *plain(lv_obj_t *parent, int x, int y, int width, int height) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, width, height);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  return o;
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_remove_flag(l, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, text);
  return l;
}

// A fixed-size box with centred text: avoids depending on measured label
// width when placing text at an exact point.
lv_obj_t *boxedLabel(lv_obj_t *parent, int cx, int cy, int boxW, int boxH,
                     const lv_font_t *font, lv_color_t color, const char *text) {
  lv_obj_t *l = label(parent, font, color, text);
  lv_obj_set_size(l, boxW, boxH);
  lv_obj_set_pos(l, cx - boxW / 2, cy - boxH / 2);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_all(l, 0, 0);
  return l;
}

lv_obj_t *bar(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t fill,
              int32_t min, int32_t max) {
  lv_obj_t *b = lv_bar_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, width, height);
  lv_obj_set_style_radius(b, height / 2, 0);
  lv_obj_set_style_bg_color(b, COL_TRACK, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(b, height / 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(b, fill, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_bar_set_range(b, min, max);
  lv_bar_set_value(b, min, LV_ANIM_OFF);
  return b;
}

lv_obj_t *button(lv_obj_t *parent, int x, int y, int width, int height, const char *text,
                 lv_event_cb_t cb) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, width, height);
  lv_obj_set_style_radius(b, 4, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x12161b), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, COL_LAMP_EDGE, 0);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);

  lv_obj_t *l = label(b, FONT_CAP, COL_FG, text);
  lv_obj_center(l);
  return b;
}

lv_obj_t *lamp(lv_obj_t *parent, int x, int y, int width, const char *text) {
  lv_obj_t *l = label(parent, FONT_CAP, COL_LAMP_TXT_OFF, text);
  lv_obj_set_size(l, width, 20);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_all(l, 2, 0);
  lv_obj_set_style_radius(l, 3, 0);
  lv_obj_set_style_bg_color(l, COL_LAMP_OFF, 0);
  lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(l, 1, 0);
  lv_obj_set_style_border_color(l, COL_LAMP_EDGE, 0);
  return l;
}

void lampState(lv_obj_t *l, bool on, bool red) {
  lv_color_t bg = red ? COL_RED : COL_AMBER;
  lv_obj_set_style_bg_color(l, on ? bg : COL_LAMP_OFF, 0);
  lv_obj_set_style_border_color(l, on ? bg : COL_LAMP_EDGE, 0);
  lv_obj_set_style_text_color(l, on ? (red ? lv_color_white() : lv_color_black())
                                    : COL_LAMP_TXT_OFF,
                              0);
}

// ------------------------------------------------------------ tach ring
void styleArc(lv_obj_t *arc, lv_color_t dim, lv_color_t lit) {
  lv_obj_remove_style_all(arc);
  lv_obj_set_size(arc, L::ARC_OBJ, L::ARC_OBJ);
  lv_obj_set_pos(arc, L::ARC_X, L::ARC_Y);
  lv_obj_remove_flag(arc, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);

  lv_obj_set_style_arc_width(arc, L::TACH_THICK, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, dim, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);

  lv_obj_set_style_arc_width(arc, L::TACH_THICK, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lit, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);

  // No knob: the needle is drawn separately.
  lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
}

void buildTach(lv_obj_t *parent) {
  w.tach = plain(parent, 0, 0, L::W, L::TOP_H);

  // Zones, as fractions of rpmMax. Recomputed whenever the config changes.
  const cfg::Data &c = cfg::get();
  const float fAmber = c.rpmAmber / c.rpmMax;
  const float fRed = c.rpmRedline / c.rpmMax;

  w.arcGreen = lv_arc_create(w.tach);
  w.arcAmber = lv_arc_create(w.tach);
  w.arcRed = lv_arc_create(w.tach);
  styleArc(w.arcGreen, COL_ZONE_GREEN, COL_GREEN);
  styleArc(w.arcAmber, COL_ZONE_AMBER, COL_AMBER);
  styleArc(w.arcRed, COL_ZONE_RED, COL_RED);

  lv_arc_set_rotation(w.arcGreen, L::ARC_ROTATION_DEG);
  lv_arc_set_rotation(w.arcAmber, L::ARC_ROTATION_DEG);
  lv_arc_set_rotation(w.arcRed, L::ARC_ROTATION_DEG);

  lv_arc_set_bg_angles(w.arcGreen, 0, (int)(fAmber * L::ARC_SWEEP_DEG));
  lv_arc_set_bg_angles(w.arcAmber, (int)(fAmber * L::ARC_SWEEP_DEG),
                       (int)(fRed * L::ARC_SWEEP_DEG));
  lv_arc_set_bg_angles(w.arcRed, (int)(fRed * L::ARC_SWEEP_DEG), L::ARC_SWEEP_DEG);

  lv_arc_set_angles(w.arcGreen, 0, 0);
  lv_arc_set_angles(w.arcAmber, 0, 0);
  lv_arc_set_angles(w.arcRed, 0, 0);

  // Ticks + numbers every 1000 rpm, drawn on the ring like the page does.
  const int tickCount = (int)(c.rpmMax / 1000.0f) + 1;
  const int n = tickCount > 9 ? 9 : tickCount;
  char buf[8];
  for (int i = 0; i < n; i++) {
    const float f = (float)i / (float)(n - 1);
    const float a = angFor(f);
    const float ca = cosf(a), sa = sinf(a);

    w.tickLine[i] = lv_line_create(w.tach);
    lv_obj_remove_style_all(w.tickLine[i]);
    lv_obj_set_pos(w.tickLine[i], 0, 0);
    lv_obj_set_size(w.tickLine[i], L::W, L::TOP_H);
    static lv_point_precise_t tickPts[9][2];  // one array per tick, never freed
    tickPts[i][0].x = L::TACH_CX + ca * L::TICK_R_IN;
    tickPts[i][0].y = L::TACH_CY + sa * L::TICK_R_IN;
    tickPts[i][1].x = L::TACH_CX + ca * L::TICK_R_OUT;
    tickPts[i][1].y = L::TACH_CY + sa * L::TICK_R_OUT;
    lv_line_set_points(w.tickLine[i], tickPts[i], 2);
    lv_obj_set_style_line_width(w.tickLine[i], 2, 0);
    lv_obj_set_style_line_color(w.tickLine[i], f >= 0.85f ? COL_RED : COL_LAMP_EDGE, 0);

    // The number shown is the rpm at this tick, in thousands.
    snprintf(buf, sizeof(buf), "%d", (int)lroundf((c.rpmMax * f) / 1000.0f));
    w.tickLabel[i] = boxedLabel(w.tach, (int)(L::TACH_CX + ca * L::TICK_R_LABEL),
                                (int)(L::TACH_CY + sa * L::TICK_R_LABEL), 42, 18, FONT_TICK,
                                COL_DIM, buf);
  }

  // Needle, then hub on top of its centre.
  w.needle = lv_line_create(w.tach);
  lv_obj_remove_style_all(w.needle);
  lv_obj_set_pos(w.needle, 0, 0);
  lv_obj_set_size(w.needle, L::W, L::TOP_H);
  g_needlePts[0].x = L::TACH_CX;
  g_needlePts[0].y = L::TACH_CY;
  g_needlePts[1].x = L::TACH_CX;
  g_needlePts[1].y = L::TACH_CY - L::NEEDLE_LEN;
  lv_line_set_points_mutable(w.needle, g_needlePts, 2);
  lv_obj_set_style_line_width(w.needle, 3, 0);
  lv_obj_set_style_line_color(w.needle, COL_WHITE, 0);
  lv_obj_set_style_line_rounded(w.needle, true, 0);

  w.hub = plain(w.tach, L::TACH_CX - L::HUB_R, L::TACH_CY - L::HUB_R, 2 * L::HUB_R,
                2 * L::HUB_R);
  lv_obj_set_style_radius(w.hub, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(w.hub, COL_HUB, 0);
  lv_obj_set_style_bg_opa(w.hub, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(w.hub, 2, 0);
  lv_obj_set_style_border_color(w.hub, COL_LAMP_EDGE, 0);

  w.rpmDigits = boxedLabel(w.tach, L::TACH_CX, L::TACH_CY - (int)(L::TACH_R * 0.30f), 320, 60,
                           FONT_TACH, COL_FG, "--");
  boxedLabel(w.tach, L::TACH_CX, L::TACH_CY - (int)(L::TACH_R * 0.02f), 80, 24, FONT_RPM_LBL,
             COL_DIM, "RPM");

  // Shift lamp, top-right (as dash.html places it).
  w.shiftLamp = label(w.tach, FONT_BODY, lv_color_hex(0x3a1414), "SHIFT");
  lv_obj_set_size(w.shiftLamp, 64, 24);
  lv_obj_set_pos(w.shiftLamp, L::W - 64 - 10, 8);
  lv_obj_set_style_text_align(w.shiftLamp, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_all(w.shiftLamp, 3, 0);
  lv_obj_set_style_radius(w.shiftLamp, 3, 0);
  lv_obj_set_style_bg_color(w.shiftLamp, lv_color_hex(0x2a0f0f), 0);
  lv_obj_set_style_bg_opa(w.shiftLamp, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(w.shiftLamp, 1, 0);
  lv_obj_set_style_border_color(w.shiftLamp, lv_color_hex(0x481a1a), 0);
}

// --------------------------------------------------------------- tiles
void buildTiles(lv_obj_t *parent) {
  const int y = L::BOTTOM_Y;
  const int h = L::BOTTOM_H;

  // ---- SPEED ------------------------------------------------------
  lv_obj_t *t0 = plain(parent, L::COL0_X, y, L::COL0_W, h);
  lv_obj_set_style_border_side(t0, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(t0, 1, 0);
  lv_obj_set_style_border_color(t0, COL_LINE, 0);

  lv_obj_t *cap = label(t0, FONT_CAP, COL_DIM, "SPEED");
  lv_obj_set_pos(cap, L::TILE_PAD_H, L::TILE_PAD_V);

  w.speedVal = label(t0, FONT_BIG, COL_FG, "--");
  lv_obj_set_pos(w.speedVal, L::TILE_PAD_H - 2, 44);

  lv_obj_t *unit = label(t0, FONT_CAP, COL_DIM, "km/h");
  lv_obj_set_pos(unit, L::TILE_PAD_H + 4, h - 44);

  w.speedPeak = label(t0, FONT_SMALL, COL_DIM, "peak --");
  lv_obj_set_pos(w.speedPeak, L::TILE_PAD_H, h - 24);

  // ---- BOOST ------------------------------------------------------
  lv_obj_t *t1 = plain(parent, L::COL1_X, y, L::COL1_W, h);
  lv_obj_set_style_border_side(t1, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(t1, 1, 0);
  lv_obj_set_style_border_color(t1, COL_LINE, 0);

  cap = label(t1, FONT_CAP, COL_DIM, "BOOST");
  lv_obj_set_pos(cap, L::TILE_PAD_H, L::TILE_PAD_V);

  w.boostVal = label(t1, FONT_BIG, COL_FG, "--");
  lv_obj_set_pos(w.boostVal, L::TILE_PAD_H - 2, 44);

  w.boostUnit = label(t1, FONT_CAP, COL_DIM, "bar");
  lv_obj_set_pos(w.boostUnit, L::TILE_PAD_H + 4, h - 44);

  w.boostBar = bar(t1, L::TILE_PAD_H, h - 62, L::COL1_W - 2 * L::TILE_PAD_H, 9, COL_GREEN, 0,
                   100);
  w.boostPeak = label(t1, FONT_SMALL, COL_DIM, "peak --");
  lv_obj_set_pos(w.boostPeak, L::TILE_PAD_H, h - 24);

  // ---- TEMPERATURES -----------------------------------------------
  lv_obj_t *t2 = plain(parent, L::COL2_X, y, L::COL2_W, h);
  lv_obj_set_style_border_side(t2, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(t2, 1, 0);
  lv_obj_set_style_border_color(t2, COL_LINE, 0);

  cap = label(t2, FONT_CAP, COL_DIM, "TEMPERATURES");
  lv_obj_set_pos(cap, L::TILE_PAD_H, L::TILE_PAD_V);

  const int tw = L::COL2_W - 2 * L::TILE_PAD_H;
  lv_obj_t *cl = label(t2, FONT_CAP, COL_DIM, "COOLANT");
  lv_obj_set_pos(cl, L::TILE_PAD_H, 40);
  w.coolVal = label(t2, FONT_TEMP, COL_FG, "--");
  lv_obj_set_pos(w.coolVal, L::TILE_PAD_H + tw - 74, 32);
  w.coolBar = bar(t2, L::TILE_PAD_H, 74, tw, 10, COL_GREEN, 0, 100);

  lv_obj_t *ol = label(t2, FONT_CAP, COL_DIM, "OIL");
  lv_obj_set_pos(ol, L::TILE_PAD_H, 104);
  w.oilVal = label(t2, FONT_TEMP, COL_FG, "--");
  lv_obj_set_pos(w.oilVal, L::TILE_PAD_H + tw - 74, 96);
  w.oilBar = bar(t2, L::TILE_PAD_H, 138, tw, 10, COL_GREEN, 0, 100);

  // ---- HALDEX -----------------------------------------------------
  lv_obj_t *t3 = plain(parent, L::COL3_X, y, L::COL3_W, h);

  cap = label(t3, FONT_CAP, COL_DIM, "HALDEX");
  lv_obj_set_pos(cap, L::TILE_PAD_H, L::TILE_PAD_V);

  w.modeVal = label(t3, FONT_MODE, COL_FG, "--");
  lv_obj_set_pos(w.modeVal, L::TILE_PAD_H, 36);

  w.haldexSub = label(t3, FONT_SMALL, COL_DIM, "coupling --");
  lv_obj_set_pos(w.haldexSub, L::TILE_PAD_H, 70);

  lv_obj_t *lr = label(t3, FONT_CAP, COL_DIM, "LOCK REQ");
  lv_obj_set_pos(lr, L::TILE_PAD_H, 94);
  w.lockReqVal = label(t3, FONT_TEMP, COL_FG, "--");
  lv_obj_set_pos(w.lockReqVal, L::COL3_W - 70, 88);

  lv_obj_t *la = label(t3, FONT_CAP, COL_DIM, "LOCK ACT");
  lv_obj_set_pos(la, L::TILE_PAD_H, 126);
  w.lockActVal = label(t3, FONT_TEMP, COL_FG, "--");
  lv_obj_set_pos(w.lockActVal, L::COL3_W - 70, 120);

  lv_obj_t *thr = label(t3, FONT_CAP, COL_FG, "THR");
  lv_obj_set_pos(thr, L::TILE_PAD_H + 4, 160);
  w.thrBar = bar(t3, L::TILE_PAD_H, 176, L::COL3_W - 2 * L::TILE_PAD_H, 12, COL_GREEN, 0, 100);
}

// ---------------------------------------------------------- status bar
void onResetPeaks(lv_event_t *) {
  g_peaks.rpm = g_peaks.speed = g_peaks.boost = NAN;
  setText(w.peakTxt, "PEAKS RESET");
}

void onToggleDay(lv_event_t *) {
  cfg::Data d = cfg::get();
  d.brightness = (d.brightness < 70.0f) ? 100.0f : 35.0f;
  cfg::apply(d);
  crowpanel::setBacklight((uint8_t)lroundf(d.brightness));
  setText(w.dayLabel, (d.brightness < 70) ? "NIGHT" : "DAY");
}

void onOpenSettings(lv_event_t *);

void buildStatusBar(lv_obj_t *parent) {
  const int y = L::H - L::STATUS_H;
  lv_obj_t *s = plain(parent, 0, y, L::W, L::STATUS_H);
  lv_obj_set_style_bg_color(s, COL_PANEL, 0);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(s, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_border_width(s, 1, 0);
  lv_obj_set_style_border_color(s, COL_LINE, 0);

  w.dot = plain(s, 9, (L::STATUS_H - 11) / 2, 11, 11);
  lv_obj_set_style_radius(w.dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(w.dot, COL_DIM, 0);
  lv_obj_set_style_bg_opa(w.dot, LV_OPA_COVER, 0);

  w.connTxt = label(s, FONT_CAP, COL_DIM, "CONNECTING");
  lv_obj_set_pos(w.connTxt, 26, 7);

  const int lampY = 5;
  w.lampBrake = lamp(s, 92, lampY, 50, "BRAKE");
  w.lampHand = lamp(s, 146, lampY, 44, "HAND");
  w.lampTc = lamp(s, 194, lampY, 32, "TC");
  w.lampAsr = lamp(s, 230, lampY, 36, "ASR");
  w.lampAwd = lamp(s, 270, lampY, 38, "AWD");

  w.peakTxt = label(s, FONT_CAP, COL_DIM, "");
  lv_obj_set_pos(w.peakTxt, 316, 7);

  w.btnReset = button(s, 520, 3, 104, 24, "RESET PEAKS", onResetPeaks);
  w.btnDay = button(s, 630, 3, 56, 24, "DAY", onToggleDay);
  w.btnCfg = button(s, 692, 3, 100, 24, "SETTINGS", onOpenSettings);
  w.dayLabel = lv_obj_get_child(w.btnDay, 0);
}

// ------------------------------------------------------------- overlay
void buildStaleOverlay(lv_obj_t *parent) {
  w.stale = plain(parent, 0, 0, L::W, L::TOP_H + L::BOTTOM_H);
  lv_obj_set_style_bg_color(w.stale, lv_color_hex(0x0a0a0a), 0);
  lv_obj_set_style_bg_opa(w.stale, 219, 0);  // rgba(10,10,10,0.86)
  lv_obj_add_flag(w.stale, LV_OBJ_FLAG_HIDDEN);

  w.staleBig = label(w.stale, FONT_BIG, COL_DIM, "NO DATA");
  lv_obj_align(w.staleBig, LV_ALIGN_CENTER, 0, -18);

  w.staleSmall = label(w.stale, FONT_BODY, COL_DIM, "waiting for /api/live ...");
  lv_obj_align(w.staleSmall, LV_ALIGN_CENTER, 0, 30);
}

// ------------------------------------------------------------ settings
cfg::Data g_edit;

struct StepCtx {
  float *field;
  float step, min, max;
  int decimals;
  lv_obj_t *valueLabel;
};
StepCtx g_steps[16];
int g_stepCount = 0;

void fmtStepper(char *buf, size_t n, const StepCtx &c) {
  if (c.decimals == 0) {
    snprintf(buf, n, "%d", (int)lroundf(*c.field));
  } else {
    snprintf(buf, n, "%.*f", c.decimals, (double)*c.field);
  }
}

void addStepper(lv_obj_t *parent, int y, const char *name, float *field, float step, float min,
                float max, int decimals) {
  if (g_stepCount >= 16) return;
  StepCtx &c = g_steps[g_stepCount++];
  c.field = field;
  c.step = step;
  c.min = min;
  c.max = max;
  c.decimals = decimals;

  lv_obj_t *l = label(parent, FONT_SMALL, lv_color_hex(0xc6ced6), name);
  lv_obj_set_pos(l, 0, y + 4);

  // [-] value [+]
  lv_obj_t *minus = button(parent, 300, y, 30, 24, "-", nullptr);
  lv_obj_add_event_cb(minus, [](lv_event_t *e) {
    auto *ctx = (StepCtx *)lv_event_get_user_data(e);
    float v = *ctx->field - ctx->step;
    if (v < ctx->min) v = ctx->min;
    *ctx->field = v;
    char buf[16];
    fmtStepper(buf, sizeof(buf), *ctx);
    setText(ctx->valueLabel, buf);
  }, LV_EVENT_CLICKED, &c);

  c.valueLabel = boxedLabel(parent, 370, y + 12, 80, 22, FONT_BODY, COL_FG, "-");

  lv_obj_t *plus = button(parent, 410, y, 30, 24, "+", nullptr);
  lv_obj_add_event_cb(plus, [](lv_event_t *e) {
    auto *ctx = (StepCtx *)lv_event_get_user_data(e);
    float v = *ctx->field + ctx->step;
    if (v > ctx->max) v = ctx->max;
    *ctx->field = v;
    char buf[16];
    fmtStepper(buf, sizeof(buf), *ctx);
    setText(ctx->valueLabel, buf);
  }, LV_EVENT_CLICKED, &c);
}

void refreshSteppers() {
  char buf[16];
  for (int i = 0; i < g_stepCount; i++) {
    fmtStepper(buf, sizeof(buf), g_steps[i]);
    setText(g_steps[i].valueLabel, buf);
  }
}

void syncTachZones() {
  const cfg::Data &c = cfg::get();
  const float fAmber = c.rpmAmber / c.rpmMax;
  const float fRed = c.rpmRedline / c.rpmMax;
  const int amberDeg = (int)(fAmber * L::ARC_SWEEP_DEG);
  const int redDeg = (int)(fRed * L::ARC_SWEEP_DEG);
  lv_arc_set_bg_angles(w.arcGreen, 0, amberDeg);
  lv_arc_set_bg_angles(w.arcAmber, amberDeg, redDeg);
  lv_arc_set_bg_angles(w.arcRed, redDeg, L::ARC_SWEEP_DEG);
}

void taEvent(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  lv_obj_t *ta = lv_event_get_target_obj(e);
  if (code == LV_EVENT_FOCUSED) {
    lv_obj_remove_flag(w.kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(w.kb, ta);
    lv_obj_move_foreground(w.kb);
  } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
    lv_keyboard_set_textarea(w.kb, nullptr);
    lv_obj_add_flag(w.kb, LV_OBJ_FLAG_HIDDEN);
  }
}

lv_obj_t *addField(lv_obj_t *parent, int y, const char *name, const char *value, bool password,
                   int width) {
  lv_obj_t *l = label(parent, FONT_SMALL, lv_color_hex(0xc6ced6), name);
  lv_obj_set_pos(l, 0, y + 8);

  lv_obj_t *ta = lv_textarea_create(parent);
  lv_obj_set_pos(ta, 160, y);
  lv_obj_set_size(ta, width, 34);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_max_length(ta, password ? 64 : 47);
  lv_textarea_set_password_mode(ta, password);
  lv_textarea_set_text(ta, value);
  lv_obj_set_style_bg_color(ta, lv_color_hex(0x0e1114), 0);
  lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(ta, COL_FG, 0);
  lv_obj_set_style_text_font(ta, FONT_BODY, 0);
  lv_obj_set_style_border_width(ta, 1, 0);
  lv_obj_set_style_border_color(ta, COL_LAMP_EDGE, 0);
  lv_obj_set_style_radius(ta, 3, 0);
  lv_obj_add_event_cb(ta, taEvent, LV_EVENT_ALL, nullptr);
  return ta;
}

lv_obj_t *g_taSsid = nullptr, *g_taPass = nullptr, *g_taHost = nullptr;
lv_obj_t *g_btnTempUnits = nullptr, *g_btnBoostUnits = nullptr;

void onTempUnits(lv_event_t *) {
  g_edit.tempF = !g_edit.tempF;
  setText(lv_obj_get_child(g_btnTempUnits, 0), g_edit.tempF ? "F" : "C");
}

void onBoostUnits(lv_event_t *) {
  g_edit.boostUnits = (g_edit.boostUnits + 1) % 3;
  static const char *const names[3] = {"bar", "mbar", "psi"};
  setText(lv_obj_get_child(g_btnBoostUnits, 0), names[g_edit.boostUnits]);
}

void onCfgSave(lv_event_t *) {
  strncpy(g_edit.ssid, lv_textarea_get_text(g_taSsid), sizeof(g_edit.ssid) - 1);
  strncpy(g_edit.pass, lv_textarea_get_text(g_taPass), sizeof(g_edit.pass) - 1);
  strncpy(g_edit.host, lv_textarea_get_text(g_taHost), sizeof(g_edit.host) - 1);
  cfg::apply(g_edit);
  crowpanel::setBacklight((uint8_t)lroundf(cfg::get().brightness));
  syncTachZones();
  showSettings(false);
}

void onCfgDefaults(lv_event_t *) {
  const cfg::Data keep = cfg::get();
  cfg::setDefaults();
  cfg::Data fresh = cfg::get();
  strncpy(fresh.ssid, keep.ssid, sizeof(fresh.ssid) - 1);
  strncpy(fresh.pass, keep.pass, sizeof(fresh.pass) - 1);
  strncpy(fresh.host, keep.host, sizeof(fresh.host) - 1);
  cfg::apply(fresh);
  g_edit = cfg::get();
  lv_textarea_set_text(g_taSsid, g_edit.ssid);
  lv_textarea_set_text(g_taPass, g_edit.pass);
  lv_textarea_set_text(g_taHost, g_edit.host);
  refreshSteppers();
  syncTachZones();
}

void onCfgClose(lv_event_t *) { showSettings(false); }

void buildSettings() {
  w.settings = lv_obj_create(lv_layer_top());
  lv_obj_remove_flag(w.settings, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(w.settings, L::W, L::H);
  lv_obj_set_pos(w.settings, 0, 0);
  lv_obj_set_style_bg_color(w.settings, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(w.settings, 214, 0);
  lv_obj_set_style_border_width(w.settings, 0, 0);
  lv_obj_set_style_pad_all(w.settings, 0, 0);
  lv_obj_add_flag(w.settings, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *card = lv_obj_create(w.settings);
  lv_obj_set_size(card, 780, 420);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_style_bg_color(card, COL_PANEL, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, COL_LINE, 0);
  lv_obj_set_style_radius(card, 6, 0);
  lv_obj_set_style_pad_all(card, 12, 0);
  lv_obj_set_scroll_dir(card, LV_DIR_VER);
  lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  int y = 0;
  lv_obj_t *h = label(card, FONT_MODE, COL_FG, "SETTINGS");
  lv_obj_set_pos(h, 0, y);
  y += 44;

  lv_obj_t *net = label(card, FONT_BODY, COL_DIM, "CONNECTION TO OPENHALDEX");
  lv_obj_set_pos(net, 0, y);
  y += 26;

  g_taSsid = addField(card, y, "SSID", g_edit.ssid, false, 240);
  y += 44;
  g_taPass = addField(card, y, "PASSWORD", g_edit.pass, true, 240);
  y += 44;
  g_taHost = addField(card, y, "HOST", g_edit.host, false, 240);
  y += 52;

  lv_obj_t *disp = label(card, FONT_BODY, COL_DIM, "DISPLAY");
  lv_obj_set_pos(disp, 0, y);
  y += 26;

  addStepper(card, y, "Brightness (%)", &g_edit.brightness, 5.0f, 10.0f, 100.0f, 0);
  y += 32;

  lv_obj_t *tu = label(card, FONT_SMALL, lv_color_hex(0xc6ced6), "Temperature units");
  lv_obj_set_pos(tu, 0, y + 4);
  g_btnTempUnits = button(card, 300, y, 40, 24, g_edit.tempF ? "F" : "C", onTempUnits);
  y += 32;

  lv_obj_t *bu = label(card, FONT_SMALL, lv_color_hex(0xc6ced6), "Boost units");
  lv_obj_set_pos(bu, 0, y + 4);
  g_btnBoostUnits = button(card, 300, y, 60, 24, "bar", onBoostUnits);
  y += 40;

  lv_obj_t *thr = label(card, FONT_BODY, COL_DIM, "THRESHOLDS");
  lv_obj_set_pos(thr, 0, y);
  y += 26;

  addStepper(card, y, "RPM max", &g_edit.rpmMax, 250, 3000, 12000, 0); y += 30;
  addStepper(card, y, "RPM amber", &g_edit.rpmAmber, 250, 1000, 12000, 0); y += 30;
  addStepper(card, y, "RPM redline", &g_edit.rpmRedline, 250, 1000, 12000, 0); y += 30;
  addStepper(card, y, "RPM shift light", &g_edit.rpmShift, 250, 1000, 12000, 0); y += 30;
  addStepper(card, y, "Coolant warn (C)", &g_edit.coolWarn, 1, 0, 200, 0); y += 30;
  addStepper(card, y, "Coolant crit (C)", &g_edit.coolCrit, 1, 0, 220, 0); y += 30;
  addStepper(card, y, "Oil warn (C)", &g_edit.oilWarn, 1, 0, 200, 0); y += 30;
  addStepper(card, y, "Oil crit (C)", &g_edit.oilCrit, 1, 0, 220, 0); y += 30;
  addStepper(card, y, "Boost max (mbar)", &g_edit.boostMax, 100, 200, 5000, 0); y += 30;
  addStepper(card, y, "Boost warn (mbar)", &g_edit.boostWarn, 100, 0, 5000, 0); y += 30;
  addStepper(card, y, "Boost crit (mbar)", &g_edit.boostCrit, 100, 0, 5000, 0); y += 40;

  button(card, 0, y, 90, 30, "DEFAULTS", onCfgDefaults);
  button(card, 600, y, 74, 30, "SAVE", onCfgSave);
  button(card, 682, y, 70, 30, "CLOSE", onCfgClose);

  w.kb = lv_keyboard_create(lv_layer_top());
  lv_obj_set_size(w.kb, L::W, 200);
  lv_obj_align(w.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(w.kb, LV_OBJ_FLAG_HIDDEN);
}

void onOpenSettings(lv_event_t *) { showSettings(true); }
void onLongPress(lv_event_t *) { showSettings(true); }

// -------------------------------------------------------------- update
float easeValue(float shown, float target, float dt) {
  if (isnan(target)) return NAN;
  if (isnan(shown)) return target;
  const float k = 1.0f - expf(-dt / kTau);
  return shown + (target - shown) * k;
}

void setSuffix(lv_obj_t *valueLabel, const char *text, lv_color_t color) {
  setText(valueLabel, text);
  setTextColor(valueLabel, color);
}

void fmtTemp(char *buf, size_t n, float celsius) {
  if (isnan(celsius)) {
    snprintf(buf, n, "--");
    return;
  }
  float v = celsius;
  if (cfg::get().tempF) v = v * 9.0f / 5.0f + 32.0f;
  snprintf(buf, n, "%d", (int)lroundf(v));
}

void fmtBoost(char *buf, size_t n, float mbar) {
  if (isnan(mbar)) {
    snprintf(buf, n, "--");
    return;
  }
  switch (cfg::get().boostUnits) {
    case cfg::BOOST_MBAR:
      snprintf(buf, n, "%d", (int)lroundf(mbar));
      break;
    case cfg::BOOST_PSI:
      snprintf(buf, n, "%.1f", (double)(mbar / 68.9476f));
      break;
    default:
      snprintf(buf, n, "%.2f", (double)(mbar / 1000.0f));
      break;
  }
}

const char *boostUnitText() {
  switch (cfg::get().boostUnits) {
    case cfg::BOOST_MBAR: return "mbar";
    case cfg::BOOST_PSI: return "psi";
    default: return "bar";
  }
}

void updateTach(const cfg::Data &c, float rpm, bool stale) {
  const bool have = !stale && !isnan(rpm);
  const float frac = have ? (rpm / c.rpmMax) : 0.0f;
  const float fAmber = c.rpmAmber / c.rpmMax;
  const float fRed = c.rpmRedline / c.rpmMax;

  const int litDeg = (int)(frac * L::ARC_SWEEP_DEG);
  const int amberDeg = (int)(fAmber * L::ARC_SWEEP_DEG);
  const int redDeg = (int)(fRed * L::ARC_SWEEP_DEG);

  // Each zone shows only the part of the sweep it owns.
  lv_arc_set_angles(w.arcGreen, 0, litDeg < amberDeg ? litDeg : amberDeg);
  lv_arc_set_angles(w.arcAmber, amberDeg, litDeg > amberDeg ? (litDeg < redDeg ? litDeg : redDeg) : amberDeg);
  lv_arc_set_angles(w.arcRed, redDeg, litDeg > redDeg ? litDeg : redDeg);

  // Needle.
  const float a = angFor(have ? frac : 0.0f);
  g_needlePts[0].x = L::TACH_CX - (int)(cosf(a) * L::NEEDLE_TAIL);
  g_needlePts[0].y = L::TACH_CY - (int)(sinf(a) * L::NEEDLE_TAIL);
  g_needlePts[1].x = L::TACH_CX + (int)(cosf(a) * L::NEEDLE_LEN);
  g_needlePts[1].y = L::TACH_CY + (int)(sinf(a) * L::NEEDLE_LEN);
  lv_obj_invalidate(w.needle);

  // Digits + shift lamp.
  const bool flash = have && (rpm >= c.rpmShift) && g_flashOn;
  char buf[16];
  if (have) {
    snprintf(buf, sizeof(buf), "%d", (int)lroundf(rpm));
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  setSuffix(w.rpmDigits, buf, flash ? COL_RED : COL_FG);

  lv_obj_set_style_bg_color(w.shiftLamp, flash ? COL_RED : lv_color_hex(0x2a0f0f), 0);
  lv_obj_set_style_border_color(w.shiftLamp, flash ? lv_color_white() : lv_color_hex(0x481a1a), 0);
  lv_obj_set_style_text_color(w.shiftLamp, flash ? lv_color_white() : lv_color_hex(0x3a1414), 0);
}

void updateReadouts(const cfg::Data &c, const telemetry::Live &live, const telemetry::Dash &d,
                    bool stale) {
  (void)live;  // only the interpolated copy is drawn
  const Values v = stale ? kBlank : g_shown;
  char buf[32];
  char b2[32];

  // ---- speed
  if (isnan(v.speed)) {
    setSuffix(w.speedVal, "--", COL_DIM);
  } else {
    snprintf(buf, sizeof(buf), "%d", (int)lroundf(v.speed));
    setSuffix(w.speedVal, buf, COL_FG);
  }
  if (isnan(g_peaks.speed)) {
    setText(w.speedPeak, "peak --");
  } else {
    snprintf(buf, sizeof(buf), "peak %d", (int)lroundf(g_peaks.speed));
    setText(w.speedPeak, buf);
  }

  // ---- boost
  fmtBoost(buf, sizeof(buf), v.boost);
  const lv_color_t bcol = colFor(v.boost, c.boostWarn, c.boostCrit);
  setSuffix(w.boostVal, buf, bcol);
  setText(w.boostUnit, boostUnitText());
  {
    int pct = 0;
    if (!isnan(v.boost)) {
      pct = (int)((v.boost / c.boostMax) * 100.0f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
    setBar(w.boostBar, pct);
    lv_obj_set_style_bg_color(w.boostBar, bcol, LV_PART_INDICATOR);
  }
  if (isnan(g_peaks.boost)) {
    setText(w.boostPeak, "peak --");
  } else {
    fmtBoost(b2, sizeof(b2), g_peaks.boost);
    snprintf(buf, sizeof(buf), "peak %s %s", b2, boostUnitText());
    setText(w.boostPeak, buf);
  }

  // ---- temperatures
  fmtTemp(buf, sizeof(buf), v.coolant);
  setSuffix(w.coolVal, buf, colFor(v.coolant, c.coolWarn, c.coolCrit));
  {
    int pct = 0;
    if (!isnan(v.coolant)) {
      pct = (int)(((v.coolant - 40.0f) / 90.0f) * 100.0f);  // 40..130 C scale
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
    setBar(w.coolBar, pct);
    lv_obj_set_style_bg_color(w.coolBar, colFor(v.coolant, c.coolWarn, c.coolCrit),
                              LV_PART_INDICATOR);
  }

  fmtTemp(buf, sizeof(buf), v.oil);
  setSuffix(w.oilVal, buf, colFor(v.oil, c.oilWarn, c.oilCrit));
  {
    int pct = 0;
    if (!isnan(v.oil)) {
      pct = (int)(((v.oil - 40.0f) / 90.0f) * 100.0f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
    setBar(w.oilBar, pct);
    lv_obj_set_style_bg_color(w.oilBar, colFor(v.oil, c.oilWarn, c.oilCrit), LV_PART_INDICATOR);
  }

  // ---- haldex
  if (d.mode >= 0 && d.mode < 6) {
    setText(w.modeVal, kModeNames[d.mode]);
  } else {
    setText(w.modeVal, "--");
  }

  if (stale) {
    setText(w.haldexSub, "coupling --");
  } else if (d.haldexCAN) {
    const char *state = d.couplingOpen ? "coupling open" : "coupling closed";
    setText(w.haldexSub, d.tempProtection ? "temp protection" : state);
  } else {
    setText(w.haldexSub, "Haldex CAN --");
  }

  if (isnan(v.lockTarget)) {
    setSuffix(w.lockReqVal, "--", COL_DIM);
  } else {
    snprintf(buf, sizeof(buf), "%d%%", (int)lroundf(v.lockTarget));
    setSuffix(w.lockReqVal, buf, COL_FG);
  }
  if (isnan(v.lockActual)) {
    setSuffix(w.lockActVal, "--", COL_DIM);
  } else {
    snprintf(buf, sizeof(buf), "%d%%", (int)lroundf(v.lockActual));
    setSuffix(w.lockActVal, buf, COL_CYAN);
  }

  {
    int pct = 0;
    if (!isnan(v.throttle)) {
      pct = (int)v.throttle;
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }
    setBar(w.thrBar, pct);
  }

  // ---- lamps
  lampState(w.lampBrake, !stale && d.brakeIn, false);
  lampState(w.lampHand, !stale && d.handbrakeIn, false);
  lampState(w.lampTc, !stale && d.tcOn, false);
  lampState(w.lampAsr, !stale && d.asrOn, false);
  lampState(w.lampAwd, !stale && d.awdWarning, true);
}

void updateConn(bool stale) {
  const telemetry::Stats s = telemetry::stats();

  if (!s.wifiUp) {
    setText(w.connTxt, "NO WIFI");
    setTextColor(w.connTxt, COL_RED);
    lv_obj_set_style_bg_color(w.dot, COL_RED, 0);
  } else if (stale) {
    setText(w.connTxt, "STALE");
    setTextColor(w.connTxt, COL_AMBER);
    lv_obj_set_style_bg_color(w.dot, COL_AMBER, 0);
  } else {
    setText(w.connTxt, "LIVE");
    setTextColor(w.connTxt, COL_GREEN);
    lv_obj_set_style_bg_color(w.dot, COL_GREEN, 0);
  }

  if (stale) {
    lv_obj_remove_flag(w.stale, LV_OBJ_FLAG_HIDDEN);
    if (!s.wifiUp) {
      setText(w.staleBig, "NO WIFI");
      setText(w.staleSmall, "check settings: SSID / password");
    } else if (!s.liveOk) {
      setText(w.staleBig, "NO DATA");
      setText(w.staleSmall, "waiting for /api/live ...");
    } else {
      setText(w.staleBig, "NO DATA");
      setText(w.staleSmall, "connection to OpenHaldex lost");
    }
  } else {
    lv_obj_add_flag(w.stale, LV_OBJ_FLAG_HIDDEN);
  }
}

}  // namespace

// ---------------------------------------------------------------- API
void begin() {
  lv_obj_t *scr = lv_screen_active();
  if (!scr) {
    // No display was registered (panel bring-up failed). Nothing in LVGL
    // is safe to touch, so leave the screen unbuilt.
    return;
  }
  lv_obj_remove_style_all(scr);
  lv_obj_set_style_bg_color(scr, COL_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  buildTach(scr);
  buildTiles(scr);
  buildStatusBar(scr);
  buildStaleOverlay(scr);

  // Long-press anywhere on the tach opens settings: the status-bar buttons
  // are only 24 px tall, which is a small target in a moving car.
  lv_obj_add_flag(w.tach, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(w.tach, onLongPress, LV_EVENT_LONG_PRESSED, nullptr);

  g_edit = cfg::get();
  buildSettings();

  crowpanel::setBacklight((uint8_t)lroundf(cfg::get().brightness));
  setText(w.dayLabel, (cfg::get().brightness < 70.0f) ? "NIGHT" : "DAY");

  g_lastTickMs = millis();
  g_lastFlashMs = millis();
}

void tick() {
  const uint32_t now = millis();
  const float dt = (float)(now - g_lastTickMs) / 1000.0f;
  g_lastTickMs = now;

  // Shift-light flash, independent of the data rate.
  if ((uint32_t)(now - g_lastFlashMs) >= kFlashMs) {
    g_lastFlashMs = now;
    g_flashOn = !g_flashOn;
  }

  const cfg::Data &c = cfg::get();
  const telemetry::Live l = telemetry::live();
  const telemetry::Dash d = telemetry::dash();
  const bool stale = !telemetry::liveFresh(c.staleMs);
  g_stale = stale;

  // Targets -> interpolated values.
  g_target.rpm = l.rpm;
  g_target.speed = l.speed;
  g_target.throttle = l.throttle;
  g_target.boost = l.boost;
  g_target.coolant = l.coolant;
  g_target.oil = l.oil;
  g_target.lockTarget = l.lockTarget;
  g_target.lockActual = l.lockActual;

  if (!stale) {
    g_shown.rpm = easeValue(g_shown.rpm, g_target.rpm, dt);
    g_shown.speed = easeValue(g_shown.speed, g_target.speed, dt);
    g_shown.throttle = easeValue(g_shown.throttle, g_target.throttle, dt);
    g_shown.boost = easeValue(g_shown.boost, g_target.boost, dt);
    g_shown.coolant = easeValue(g_shown.coolant, g_target.coolant, dt * 0.25f);
    g_shown.oil = easeValue(g_shown.oil, g_target.oil, dt * 0.25f);
    g_shown.lockTarget = easeValue(g_shown.lockTarget, g_target.lockTarget, dt);
    g_shown.lockActual = easeValue(g_shown.lockActual, g_target.lockActual, dt);

    // Peaks come from the raw values, not the eased ones.
    if (!isnan(l.rpm)) g_peaks.rpm = isnan(g_peaks.rpm) ? l.rpm : fmaxf(g_peaks.rpm, l.rpm);
    if (!isnan(l.speed))
      g_peaks.speed = isnan(g_peaks.speed) ? l.speed : fmaxf(g_peaks.speed, l.speed);
    if (!isnan(l.boost))
      g_peaks.boost = isnan(g_peaks.boost) ? l.boost : fmaxf(g_peaks.boost, l.boost);
  }

  updateTach(c, g_shown.rpm, stale);
  updateReadouts(c, l, d, stale);
  updateConn(stale);
}

void showSettings(bool visible) {
  g_settingsOpen = visible;
  if (!w.settings) return;
  if (visible) {
    g_edit = cfg::get();
    lv_textarea_set_text(g_taSsid, g_edit.ssid);
    lv_textarea_set_text(g_taPass, g_edit.pass);
    lv_textarea_set_text(g_taHost, g_edit.host);
    lv_obj_remove_flag(w.settings, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(w.settings);
    lv_obj_add_flag(w.kb, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_keyboard_set_textarea(w.kb, nullptr);
    lv_obj_add_flag(w.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(w.settings, LV_OBJ_FLAG_HIDDEN);
  }
}

bool settingsOpen() { return g_settingsOpen; }

}  // namespace ui
