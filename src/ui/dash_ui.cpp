// =====================================================================
// dash_ui.cpp - the LVGL dashboard
//
// Widget map (see theme.h for the numbers):
//
//   +--------------------------------------------------------------+
//   |  BOOST  [###############...................]        1.55     |
//   |  REVS   [########################.......]           6800     |  top 252 px
//   |         1   2   3   4   5   6   7   x1000             bar    |
//   +------------------+-----------------+-------------------------+
//   |  SPEED           | TEMPERATURES    |  HALDEX                 |  bottom 197 px
//   |  123 km/h        |  cool / oil     |  mode, lock req/act, thr |
//   +------------------+-----------------+-------------------------+
//   | * LIVE  BRAKE HAND TC ASR AWD   peaks   RESET  DAY  SETTINGS | status 30 px
//   +--------------------------------------------------------------+
//
// The top band used to be a tach ring (3 anti-aliased arcs, 9 ticks, a
// needle and a hub). On this board that cost ~58 ms of a 61 ms frame - it
// was the entire frame budget - so it was replaced by two bars: a bar's
// moving edge is an opaque rectangle, and only the cells that crossed the
// edge are repainted. See docs/redesign-bars.md.
//
// Values come from net/datafeed.h, which merges the C6's ESP-NOW broadcast
// with its HTTP API, and nothing is drawn for a value that has never
// arrived ("--" instead of 0).
// =====================================================================
#include "ui/dash_ui.h"

#include <Arduino.h>
#include <cmath>
#include <cstring>

#include "hal/crowpanel.h"
#include "net/telemetry.h"
#include "net/datafeed.h"
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
  lv_obj_t *top = nullptr;  // the whole top band; long-press opens settings

  // The two bars, their cells, and the row of numbers under the rev bar.
  // A group per bar so the debug mask can hide them one at a time.
  lv_obj_t *boostGrp = nullptr, *revGrp = nullptr, *ticksGrp = nullptr, *bigGrp = nullptr;
  lv_obj_t *boostCell[L::BAR_SEG] = {};
  lv_obj_t *revCell[L::BAR_SEG] = {};
  lv_obj_t *tickLabel[12] = {};  // one per thousand rpm, minus both ends
  lv_obj_t *tickUnit = nullptr;  // "x1000", at the right-hand end
  int tickLabels = 0;

  lv_obj_t *rpmVal = nullptr, *boostVal = nullptr, *boostUnit = nullptr;
  lv_obj_t *rpmCap = nullptr, *boostCap = nullptr;

  lv_obj_t *speedVal = nullptr, *speedPeak = nullptr;
  lv_obj_t *coolVal = nullptr, *coolBar = nullptr, *oilVal = nullptr, *oilBar = nullptr;
  lv_obj_t *modeVal = nullptr, *haldexSub = nullptr;
  lv_obj_t *lockReqVal = nullptr, *lockActVal = nullptr, *thrBar = nullptr;

  lv_obj_t *dot = nullptr, *connTxt = nullptr, *peakTxt = nullptr;
  lv_obj_t *lampBrake = nullptr, *lampHand = nullptr, *lampTc = nullptr, *lampAsr = nullptr,
           *lampAwd = nullptr;
  lv_obj_t *btnReset = nullptr, *btnDay = nullptr, *btnCfg = nullptr;
  lv_obj_t *dayLabel = nullptr;

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

// 0xFFFF = draw everything (see dash_ui.h).
uint16_t g_dbgMask = 0xFFFF;

// Bench counters for "the screen went stale": how often, and how old the data
// was when it did.
uint32_t g_staleEvents = 0;
uint32_t g_maxStaleAgeMs = 0;
bool dbg(uint16_t bit) { return (g_dbgMask & bit) != 0; }

uint32_t g_barsUs = 0, g_readoutsUs = 0, g_connUs = 0, g_calls = 0;
bool g_dbgHideAll = false;
uint32_t g_lastFlashMs = 0;
uint32_t g_lastTickMs = 0;
bool g_settingsOpen = false;
bool g_stale = true;

// Which cells of each bar are currently lit: 1 lit, 0 dark, -1 unknown.
// -1 is the state after start-up and after the zone colours change, so the
// first update after that repaints every cell instead of assuming dark.
int8_t g_revLit[L::BAR_SEG];
int8_t g_boostLit[L::BAR_SEG];

// Lit/unlit colour per rev cell, from the rpm zones. Fixed until the
// redline moves, so a cell's colour is decided once per settings change, not
// once per frame.
struct CellColour {
  lv_color_t lit, unlit;
};
CellColour g_revCol[L::BAR_SEG];

// ------------------------------------------------------------- helpers
void setText(lv_obj_t *label, const char *text) {
  const char *cur = lv_label_get_text(label);
  if (!cur || strcmp(cur, text) != 0) lv_label_set_text(label, text);
}

bool sameColor(lv_color_t a, lv_color_t b) {
  return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

// Setting a style always invalidates the object, even when the value is
// identical. On a 30 Hz dashboard that is pure repaint cost, so every style
// write goes through one of these guards.
void setTextColor(lv_obj_t *label, lv_color_t c) {
  if (!sameColor(lv_obj_get_style_text_color(label, 0), c)) {
    lv_obj_set_style_text_color(label, c, 0);
  }
}

void setBgColor(lv_obj_t *obj, lv_color_t c, lv_style_selector_t sel) {
  if (!sameColor(lv_obj_get_style_bg_color(obj, sel), c)) {
    lv_obj_set_style_bg_color(obj, c, sel);
  }
}

void setBorderColor(lv_obj_t *obj, lv_color_t c) {
  if (!sameColor(lv_obj_get_style_border_color(obj, 0), c)) {
    lv_obj_set_style_border_color(obj, c, 0);
  }
}

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

// Like plain() but paints its own background, so a repaint stays inside the
// container instead of reaching back to whatever is underneath.
lv_obj_t *opaque(lv_obj_t *parent, int x, int y, int width, int height, lv_color_t bg) {
  lv_obj_t *o = plain(parent, x, y, width, height);
  lv_obj_set_style_bg_color(o, bg, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
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

// Dynamic text must never use LV_SIZE_CONTENT: a changing self-size marks
// the layout dirty, and LVGL then repaints the whole parent tile. Every
// label whose text changes gets a fixed box instead.
lv_obj_t *fixedLabel(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *font,
                     lv_color_t color, const char *text) {
  lv_obj_t *l = label(parent, font, color, text);
  lv_obj_set_size(l, w, h);
  lv_obj_set_pos(l, x, y);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
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

// Called every frame for five lamps; keep the previous state so an
// unchanged lamp costs nothing at all.
struct LampState {
  lv_obj_t *obj = nullptr;
  bool on = false;
  bool red = false;
  bool valid = false;
};
LampState g_lampState[6];

void lampState(lv_obj_t *l, bool on, bool red) {
  LampState *st = nullptr;
  for (LampState &c : g_lampState) {
    if (c.obj == l) {
      st = &c;
      break;
    }
    if (c.obj == nullptr) {
      c.obj = l;
      st = &c;
      break;
    }
  }
  if (!st) return;
  if (st->valid && st->on == on && st->red == red) return;
  st->on = on;
  st->red = red;
  st->valid = true;

  const lv_color_t bg = red ? COL_RED : COL_AMBER;
  setBgColor(l, on ? bg : COL_LAMP_OFF, 0);
  setBorderColor(l, on ? bg : COL_LAMP_EDGE);
  setTextColor(l, on ? (red ? lv_color_white() : lv_color_black()) : COL_LAMP_TXT_OFF);
}

// ---------------------------------------------------------------- gauge
// Everything that depends on the rpm/boost scale: which zone each rev cell
// belongs to, and the numbers along the bar. Called once at build time and
// again whenever the settings change the scale.
void syncGauge() {
  const cfg::Data &c = cfg::get();
  const float fAmber = c.rpmAmber / c.rpmMax;
  const float fRed = c.rpmRedline / c.rpmMax;

  for (int i = 0; i < L::BAR_SEG; i++) {
    // Each cell takes the colour of the zone its centre falls in, so the
    // amber and red zones are visible as dark tints even before they light.
    const float f = (i + 0.5f) / (float)L::BAR_SEG;
    CellColour col{COL_GREEN, COL_ZONE_GREEN};
    if (f >= fRed) {
      col = {COL_RED, COL_ZONE_RED};
    } else if (f >= fAmber) {
      col = {COL_AMBER, COL_ZONE_AMBER};
    }
    g_revCol[i] = col;
    g_revLit[i] = -1;  // force the next update to paint this cell
  }

  // One number per 1000 rpm. The left end (0) has no number and the top of
  // the scale is where the unit tag sits, so the count is perK - 1.
  const int perK = (int)lroundf(c.rpmMax / 1000.0f);
  int n = perK - 1;
  if (n < 1) n = 1;
  if (n > (int)(sizeof(w.tickLabel) / sizeof(w.tickLabel[0]))) {
    n = (int)(sizeof(w.tickLabel) / sizeof(w.tickLabel[0]));
  }
  const int labelW = 40;
  char buf[8];
  for (int i = 0; i < n; i++) {
    snprintf(buf, sizeof(buf), "%d", i + 1);  // thousands
    setText(w.tickLabel[i], buf);
    const float f = (float)((i + 1) * 1000) / c.rpmMax;
    lv_obj_set_x(w.tickLabel[i], (int)lroundf(f * L::BAR_W) - labelW / 2);
    lv_obj_remove_flag(w.tickLabel[i], LV_OBJ_FLAG_HIDDEN);
  }
  for (int i = n; i < (int)(sizeof(w.tickLabel) / sizeof(w.tickLabel[0])); i++) {
    lv_obj_add_flag(w.tickLabel[i], LV_OBJ_FLAG_HIDDEN);
  }
  w.tickLabels = n;

  // "x1000" only means something if the scale lands on whole thousands.
  if (fmodf(c.rpmMax, 1000.0f) == 0.0f) {
    lv_obj_remove_flag(w.tickUnit, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(w.tickUnit, LV_OBJ_FLAG_HIDDEN);
  }
}

// The two bars: 40 opaque cells each, painted once at build and then only
// ever recoloured where the fill edge has moved. Nothing is resized, moved
// or re-laid-out, which is what keeps a frame cheap.
void buildBars(lv_obj_t *parent) {
  w.top = opaque(parent, 0, 0, L::W, L::TOP_H, COL_BG);

  // One group per bar so the debug mask can switch each one off.
  w.boostGrp = plain(w.top, L::BAR_X, L::BOOST_BAR_Y, L::BAR_W, L::BOOST_BAR_H);
  w.revGrp = plain(w.top, L::BAR_X, L::REV_BAR_Y, L::BAR_W, L::REV_BAR_H);
  w.ticksGrp = plain(w.top, L::BAR_X, L::TICK_ROW_Y, L::BAR_W, L::TICK_ROW_H);
  w.bigGrp = plain(w.top, L::VAL_X, 0, L::VAL_W, L::TOP_H);

  for (int i = 0; i < L::BAR_SEG; i++) {
    const int x0 = (i * L::BAR_W) / L::BAR_SEG;
    const int x1 = ((i + 1) * L::BAR_W) / L::BAR_SEG - L::BAR_GAP;
    w.boostCell[i] = opaque(w.boostGrp, x0, 0, x1 - x0, L::BOOST_BAR_H, COL_TRACK);
    w.revCell[i] = opaque(w.revGrp, x0, 0, x1 - x0, L::REV_BAR_H, COL_ZONE_GREEN);
    g_boostLit[i] = -1;
  }

  // Numbers along the rev bar, plus the unit tag at its right-hand end.
  for (int i = 0; i < (int)(sizeof(w.tickLabel) / sizeof(w.tickLabel[0])); i++) {
    w.tickLabel[i] = fixedLabel(w.ticksGrp, 0, 0, 40, L::TICK_ROW_H, FONT_TICK, COL_DIM, "");
    lv_obj_set_style_text_align(w.tickLabel[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(w.tickLabel[i], LV_OBJ_FLAG_HIDDEN);
  }
  w.tickUnit =
      fixedLabel(w.ticksGrp, L::BAR_W - 48, 0, 48, L::TICK_ROW_H, FONT_CAP, COL_DIM, "x1000");
  lv_obj_set_style_text_align(w.tickUnit, LV_TEXT_ALIGN_RIGHT, 0);

  // Rule between the bars and the numbers they belong to.
  opaque(w.top, L::VAL_DIV_X, 0, 1, L::TOP_H, COL_LINE);

  // ---- right column: BOOST over REVS ------------------------------
  w.boostCap = fixedLabel(w.bigGrp, 0, L::VAL_CAP_Y, L::VAL_W, 18, FONT_CAP, COL_DIM, "BOOST");
  w.boostVal = fixedLabel(w.bigGrp, 0, L::VAL_BOOST_Y, L::VAL_W, 70, FONT_NUM, COL_FG, "--");
  w.boostUnit = fixedLabel(w.bigGrp, 0, L::VAL_UNIT_Y, L::VAL_W, 20, FONT_SMALL, COL_DIM, "bar");
  opaque(w.bigGrp, 0, L::VAL_DIV_Y, L::VAL_W, 1, COL_LINE);
  w.rpmCap = fixedLabel(w.bigGrp, 0, L::VAL_RPM_Y - 16, L::VAL_W, 18, FONT_CAP, COL_DIM, "RPM");
  w.rpmVal = fixedLabel(w.bigGrp, 0, L::VAL_RPM_Y, L::VAL_W, 90, FONT_RPM, COL_FG, "--");

  // Right-aligned to the panel's right margin, like the mock-up.
  lv_obj_t *right[3] = {w.boostVal, w.boostUnit, w.rpmVal};
  for (int i = 0; i < 3; i++) {
    lv_obj_set_style_text_align(right[i], LV_TEXT_ALIGN_RIGHT, 0);
  }

  syncGauge();
}

// --------------------------------------------------------------- tiles
// Three tiles now. The BOOST tile is gone: boost is the bar at the top of
// the screen plus the value beside it, and showing the same number in two
// places was duplicating it.
void buildTiles(lv_obj_t *parent) {
  const int y = L::BOTTOM_Y;
  const int h = L::BOTTOM_H;

  // ---- SPEED ------------------------------------------------------
  lv_obj_t *t0 = opaque(parent, L::COL0_X, y, L::COL0_W, h, COL_BG);
  lv_obj_set_style_border_side(t0, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(t0, 1, 0);
  lv_obj_set_style_border_color(t0, COL_LINE, 0);

  lv_obj_t *cap = label(t0, FONT_CAP, COL_DIM, "SPEED");
  lv_obj_set_pos(cap, L::TILE_PAD_H, L::TILE_PAD_V);

  w.speedVal = fixedLabel(t0, L::TILE_PAD_H - 2, 44, 132, 58, FONT_BIG, COL_FG, "--");

  fixedLabel(t0, L::TILE_PAD_H + 4, h - 44, 60, 18, FONT_CAP, COL_DIM, "km/h");

  w.speedPeak = fixedLabel(t0, L::TILE_PAD_H, h - 24, L::COL0_W - 2 * L::TILE_PAD_H, 18,
                           FONT_SMALL, COL_DIM, "peak --");

  // ---- TEMPERATURES -----------------------------------------------
  lv_obj_t *t1 = opaque(parent, L::COL1_X, y, L::COL1_W, h, COL_BG);
  lv_obj_set_style_border_side(t1, LV_BORDER_SIDE_RIGHT, 0);
  lv_obj_set_style_border_width(t1, 1, 0);
  lv_obj_set_style_border_color(t1, COL_LINE, 0);

  cap = label(t1, FONT_CAP, COL_DIM, "TEMPERATURES");
  lv_obj_set_pos(cap, L::TILE_PAD_H, L::TILE_PAD_V);

  const int tw = L::COL1_W - 2 * L::TILE_PAD_H;
  lv_obj_t *cl = label(t1, FONT_CAP, COL_DIM, "COOLANT");
  lv_obj_set_pos(cl, L::TILE_PAD_H, 40);
  w.coolVal = fixedLabel(t1, L::TILE_PAD_H + tw - 92, 30, 92, 30, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.coolVal, LV_TEXT_ALIGN_RIGHT, 0);
  w.coolBar = bar(t1, L::TILE_PAD_H, 74, tw, 10, COL_GREEN, 0, 100);

  lv_obj_t *ol = label(t1, FONT_CAP, COL_DIM, "OIL");
  lv_obj_set_pos(ol, L::TILE_PAD_H, 104);
  w.oilVal = fixedLabel(t1, L::TILE_PAD_H + tw - 92, 94, 92, 30, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.oilVal, LV_TEXT_ALIGN_RIGHT, 0);
  w.oilBar = bar(t1, L::TILE_PAD_H, 138, tw, 10, COL_GREEN, 0, 100);

  // ---- HALDEX -----------------------------------------------------
  lv_obj_t *t2 = opaque(parent, L::COL2_X, y, L::COL2_W, h, COL_BG);

  cap = label(t2, FONT_CAP, COL_DIM, "HALDEX");
  lv_obj_set_pos(cap, L::TILE_PAD_H, L::TILE_PAD_V);

  w.modeVal = fixedLabel(t2, L::TILE_PAD_H, 36, L::COL2_W - 2 * L::TILE_PAD_H, 36, FONT_MODE,
                         COL_FG, "--");

  w.haldexSub = fixedLabel(t2, L::TILE_PAD_H, 70, L::COL2_W - 2 * L::TILE_PAD_H, 20, FONT_SMALL,
                           COL_DIM, "coupling --");

  lv_obj_t *lr = label(t2, FONT_CAP, COL_DIM, "LOCK REQ");
  lv_obj_set_pos(lr, L::TILE_PAD_H, 94);
  w.lockReqVal = fixedLabel(t2, L::COL2_W - 110, 86, 100, 30, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.lockReqVal, LV_TEXT_ALIGN_RIGHT, 0);

  lv_obj_t *la = label(t2, FONT_CAP, COL_DIM, "LOCK ACT");
  lv_obj_set_pos(la, L::TILE_PAD_H, 126);
  w.lockActVal = fixedLabel(t2, L::COL2_W - 110, 118, 100, 30, FONT_TEMP, COL_FG, "--");
  lv_obj_set_style_text_align(w.lockActVal, LV_TEXT_ALIGN_RIGHT, 0);

  lv_obj_t *thr = label(t2, FONT_CAP, COL_FG, "THR");
  lv_obj_set_pos(thr, L::TILE_PAD_H + 4, 160);
  w.thrBar = bar(t2, L::TILE_PAD_H, 176, L::COL2_W - 2 * L::TILE_PAD_H, 12, COL_GREEN, 0, 100);
}

// ---------------------------------------------------------- status bar
void onResetPeaks(lv_event_t *) {
  g_peaks.rpm = g_peaks.speed = g_peaks.boost = NAN;
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

  // 92 px holds the widest state word ("CONNECTING") without resizing.
  w.connTxt = fixedLabel(s, 26, 8, 62, 18, FONT_CAP, COL_DIM, "CONNECT");

  const int lampY = 5;
  w.lampBrake = lamp(s, 92, lampY, 50, "BRAKE");
  w.lampHand = lamp(s, 146, lampY, 50, "HAND");
  w.lampTc = lamp(s, 200, lampY, 32, "TC");
  w.lampAsr = lamp(s, 236, lampY, 40, "ASR");
  w.lampAwd = lamp(s, 280, lampY, 42, "AWD");

  w.peakTxt = fixedLabel(s, 316, 8, 200, 18, FONT_CAP, COL_DIM, "");

  w.btnReset = button(s, 516, 3, 104, 24, "RESET PEAKS", onResetPeaks);
  w.btnDay = button(s, 626, 3, 56, 24, "DAY", onToggleDay);
  w.btnCfg = button(s, 688, 3, 104, 24, "SETTINGS", onOpenSettings);
  w.dayLabel = lv_obj_get_child(w.btnDay, 0);
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
  syncGauge();  // the zones and the numbers move with the scale
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
  syncGauge();  // the zones and the numbers move with the scale
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

// The bars, at the full render rate: the eye tracks the rev bar the way it
// used to track the needle, and the boost bar fills as the throttle goes on.
//
// A cell is only touched when its state actually changes, so a frame that
// moves the fill edge by one cell repaints exactly one 12x142 px rectangle
// instead of an arc. setBgColor() is the same-value guard used everywhere
// else, so even the state comparison is belt and braces.
void updateBars(const cfg::Data &c, float rpm, float boost) {
  if (dbg(1)) {
    const float frac = isnan(rpm) ? 0.0f : (rpm / c.rpmMax);
    int lit = (int)lroundf(frac * L::BAR_SEG);
    if (lit < 0) lit = 0;
    if (lit > L::BAR_SEG) lit = L::BAR_SEG;
    for (int i = 0; i < L::BAR_SEG; i++) {
      const int8_t want = (i < lit) ? 1 : 0;
      if (g_revLit[i] != want) {
        g_revLit[i] = want;
        setBgColor(w.revCell[i], want ? g_revCol[i].lit : g_revCol[i].unlit, 0);
      }
    }
  }

  if (dbg(2)) {
    // The bar itself is one colour; the number beside it carries the warning
    // colour, so a single-colour bar does not lose the overboost cue.
    const float frac = isnan(boost) ? 0.0f : (boost / c.boostMax);
    int lit = (int)lroundf(frac * L::BAR_SEG);
    if (lit < 0) lit = 0;
    if (lit > L::BAR_SEG) lit = L::BAR_SEG;
    for (int i = 0; i < L::BAR_SEG; i++) {
      const int8_t want = (i < lit) ? 1 : 0;
      if (g_boostLit[i] != want) {
        g_boostLit[i] = want;
        setBgColor(w.boostCell[i], want ? COL_WHITE : COL_TRACK, 0);
      }
    }
  }
}

// The two big numbers and the boost unit. Both labels are only repainted
// when their text or colour changes, so this costs nothing on the frames
// where the value has not moved.
void updateGaugeText(const cfg::Data &c, float rpm, float boost) {
  if (!dbg(8)) return;

  // A missing rpm shows as "--"; a stale one keeps its last value, because
  // going stale is the status light's job.
  char buf[24];
  const bool have = !isnan(rpm);
  if (have) {
    snprintf(buf, sizeof(buf), "%d", (int)lroundf(rpm));
  } else {
    snprintf(buf, sizeof(buf), "--");
  }
  // Shift cue: the rev readout flashes red. The old corner SHIFT box is
  // gone - the value column holds the numbers now - and a 96 px number
  // flashing is a bigger cue than a 64 px box was.
  const bool flash = have && (rpm >= c.rpmShift) && g_flashOn;
  setSuffix(w.rpmVal, buf, flash ? COL_RED : COL_FG);

  fmtBoost(buf, sizeof(buf), boost);
  setSuffix(w.boostVal, buf, colFor(boost, c.boostWarn, c.boostCrit));
  setText(w.boostUnit, boostUnitText());
}

void updateReadouts(const cfg::Data &c, const telemetry::Live &live, const telemetry::Dash &d) {
  (void)live;  // only the interpolated copy is drawn
  if (!dbg(16)) return;
  // The last known values stay on the glass when the data goes stale (the
  // status light says so); "--" is reserved for values never received, which
  // is what NaN means.
  const Values v = g_shown;
  char buf[32];

  // The two peaks that have no tile of their own. Speed's peak is in the
  // SPEED tile, where it has always been.
  {
    char rp[16] = "--", bp[16] = "--";
    if (!isnan(g_peaks.rpm)) snprintf(rp, sizeof(rp), "%d", (int)lroundf(g_peaks.rpm));
    if (!isnan(g_peaks.boost)) {
      char b[16];
      fmtBoost(b, sizeof(b), g_peaks.boost);
      snprintf(bp, sizeof(bp), "%s %s", b, boostUnitText());
    }
    snprintf(buf, sizeof(buf), "rpm %s   boost %s", rp, bp);
    setText(w.peakTxt, buf);
  }

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
    setBgColor(w.coolBar, colFor(v.coolant, c.coolWarn, c.coolCrit), LV_PART_INDICATOR);
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
    setBgColor(w.oilBar, colFor(v.oil, c.oilWarn, c.oilCrit), LV_PART_INDICATOR);
  }

  // ---- haldex
  if (d.mode >= 0 && d.mode < 6) {
    setText(w.modeVal, kModeNames[d.mode]);
  } else {
    setText(w.modeVal, "--");
  }

  if (d.haldexCAN) {
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

  // ---- lamps. Last known state is held, like the readouts above: the amber
  // status light is what tells the driver the data is no longer fresh.
  lampState(w.lampBrake, d.brakeIn, false);
  lampState(w.lampHand, d.handbrakeIn, false);
  lampState(w.lampTc, d.tcOn, false);
  lampState(w.lampAsr, d.asrOn, false);
  lampState(w.lampAwd, d.awdWarning, true);
}

void updateConn(bool stale) {
  if (!dbg(32)) return;
  const telemetry::Stats s = telemetry::stats();

  // Staleness now lives entirely in the status light: the dashboard keeps
  // showing the last values it had, and this is the only thing that changes.
  //
  // Two sources feed it now, so "no WiFi" means *neither* of them is alive:
  // ESP-NOW needs no association, so packets arriving prove the C6 is there
  // even when our own station has dropped off its access point.
  if (!s.wifiUp && !feed::onNow()) {
    setText(w.connTxt, "NO WIFI");
    setTextColor(w.connTxt, COL_RED);
    setBgColor(w.dot, COL_RED, 0);
  } else if (stale) {
    // "WAIT" until one of the two has delivered something at all.
    setText(w.connTxt, feed::source() == feed::Source::None ? "WAIT" : "STALE");
    setTextColor(w.connTxt, COL_AMBER);
    setBgColor(w.dot, COL_AMBER, 0);
  } else {
    setText(w.connTxt, "LIVE");
    setTextColor(w.connTxt, COL_GREEN);
    setBgColor(w.dot, COL_GREEN, 0);
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

  buildBars(scr);
  buildTiles(scr);
  buildStatusBar(scr);

  // Long-press the top band opens settings, as the tach ring used to: it is
  // the whole top half of the screen, which beats the 24 px status-bar
  // buttons as a target in a moving car. The rev bar is inside it.
  lv_obj_add_flag(w.top, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(w.top, onLongPress, LV_EVENT_LONG_PRESSED, nullptr);

  g_edit = cfg::get();
  buildSettings();

  crowpanel::setBacklight((uint8_t)lroundf(cfg::get().brightness));
  setText(w.dayLabel, (cfg::get().brightness < 70.0f) ? "NIGHT" : "DAY");

  g_lastTickMs = millis();
  g_lastFlashMs = millis();
}

void applyDebugMask() {
  // The bars are groups, so a group is one hide/show rather than 40 of them.
  struct Group {
    lv_obj_t *obj;
    uint16_t bit;
  };
  const Group groups[4] = {{w.revGrp, 1}, {w.boostGrp, 2}, {w.ticksGrp, 4}, {w.bigGrp, 8}};
  for (const Group &g : groups) {
    if (!g.obj) continue;
    if (dbg(g.bit)) {
      lv_obj_remove_flag(g.obj, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(g.obj, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (!w.top) return;
  if (g_dbgHideAll) {
    lv_obj_add_flag(w.top, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(w.top, LV_OBJ_FLAG_HIDDEN);
  }
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
  // The merge point: ESP-NOW while its packets are arriving, HTTP for
  // coolant and for the lamps the packet does not carry. See net/datafeed.h.
  const telemetry::Live l = feed::live();
  const telemetry::Dash d = feed::dash();
  const bool stale = !feed::fresh(c.staleMs);
  // The screen's own idea of "stale" is what the user sees, so make every
  // transition visible on the console together with how old the data was.
  static bool lastStale = false;
  static uint32_t staleEvents = 0;
  static uint32_t maxAgeMs = 0;
  g_maxStaleAgeMs = maxAgeMs;
  if (stale) {
    const uint32_t age = l.ageMs;
    if (age != UINT32_MAX && age > maxAgeMs) maxAgeMs = age;
  }
  if (stale != lastStale) {
    lastStale = stale;
    if (stale) {
      staleEvents++;
      g_staleEvents = staleEvents;
    }
    Serial.printf("[ui] %s (data age %lu ms, source %s, event %lu)\n",
                  stale ? "STALE" : "LIVE", (unsigned long)l.ageMs, feed::sourceName(),
                  (unsigned long)staleEvents);
  }
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

  uint32_t t0 = micros();
  // The bars and the big numbers run at the full rate: the rev bar's edge is
  // what the eye tracks now that the needle is gone.
  updateBars(c, g_shown.rpm, g_shown.boost);
  updateGaugeText(c, g_shown.rpm, g_shown.boost);
  uint32_t t1 = micros();
  // The tiles carry values that only arrive at 10 Hz and are already eased,
  // so repainting them 30 times a second buys nothing.
  static uint32_t lastReadouts = 0;
  if (dbg(16) && (uint32_t)(now - lastReadouts) >= 100) {
    lastReadouts = now;
    updateReadouts(c, l, d);
  }
  uint32_t t2 = micros();
  if (dbg(32)) updateConn(stale);
  uint32_t t3 = micros();

  g_barsUs += t1 - t0;
  g_readoutsUs += t2 - t1;
  g_connUs += t3 - t2;
  g_calls++;
}

uint32_t staleEvents() { return g_staleEvents; }
uint32_t maxStaleAgeMs() { return g_maxStaleAgeMs; }

void setDebugMask(uint16_t mask) {
  const bool wasHidden = g_dbgHideAll;
  g_dbgMask = mask;
  g_dbgHideAll = (mask == 0);
  (void)wasHidden;
  applyDebugMask();
  lv_obj_invalidate(lv_screen_active());
}

uint16_t debugMask() { return g_dbgMask; }

Timings takeTimings() {
  Timings t{g_barsUs, g_readoutsUs, g_connUs, g_calls};
  g_barsUs = g_readoutsUs = g_connUs = g_calls = 0;
  return t;
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
