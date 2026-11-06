// =====================================================================
// raw_ui.cpp - the raw telemetry page
//
// One page, three columns, no interpretation. It exists because two
// questions about this car cannot be answered from the dashboard: why the
// BRAKE and HAND lamps are dark when the C6's JSON says the handbrake is on,
// and why the packet carries a steering angle that the JSON reports as null.
// Both are visible here as raw numbers.
//
// The rule the whole page follows: a field the C6 marks as "no value"
// (OHX_NONE8 / OHX_NONE16 / OHX_NONE_T, or a JSON null) shows "--". It never
// shows 0, because "sensor absent" and "sensor reads zero" are different
// facts and this page is where that difference is read.
//
// It is a page of its own, not a dashboard widget, and it goes on
// lv_layer_top() like the settings card. The dashboard tree is hidden - not
// merely covered - while it is up: LVGL 9 has no "covered by an opaque
// object" skip, so anything left visible underneath is redrawn with it.
//
// Rows are fixed-size clipped labels on purpose. A label that sizes itself
// to its text marks the layout dirty on every change and takes the whole
// parent panel with it; that is the same trap the dashboard hit in settings.
// =====================================================================
#include "ui/raw_ui.h"

#include <Arduino.h>
#include <lvgl.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app/capability.h"
#include "version.h"
#include "app/config.h"
#include "net/can_relay.h"
#include "net/datafeed.h"
#include "net/ohx_now.h"
#include "net/telemetry.h"
#include "ohx_proto.h"
#include "theme.h"
#include "ui/dash_ui.h"

namespace rawui {
namespace {

using ohxnow::Status;

// ------------------------------------------------------------- geometry
constexpr int kPanelY = 36;
constexpr int kPanelH = LCD_V_RES - kPanelY - 6;  // 438
constexpr int kColX[3] = {6, 272, 538};
constexpr int kColW = 258;
constexpr int kPadX = 6;
constexpr int kNameW = 112;
constexpr int kValueW = 122;
constexpr int kRowH = 18;
constexpr int kHeadH = 20;
constexpr int kFlagH = 34;  // two wrapped lines

// The packet arrives at 10 Hz, so there is no information in refreshing
// faster than that.
constexpr uint32_t kRefreshMs = 100;

lv_obj_t *g_root = nullptr;
lv_obj_t *g_title = nullptr;
lv_obj_t *g_holdLabel = nullptr;
bool g_visible = false;
bool g_hold = false;
uint32_t g_lastMs = 0;

// ---------------------------------------------------------------- fields
// One entry per row. The array is the layout: adding a field means adding a
// name here and its row definition below.
enum Field : uint8_t {
  F_HDR, F_FW, F_GEN, F_MODE, F_NAME, F_SRC, F_CHRSSI, F_PKTS, F_SEQNONCE, F_REJDUP, F_SENDERS,
  F_CAP_GEN, F_CAP_BOOST, F_CAP_SUPPLY, F_CAP_TEMPS, F_CAP_STEER, F_CAP_LAYOUT,
  F_RELAY_RATE, F_RELAY_AIR, F_RELAY_SRC,
  F_RPM, F_SPEED, F_THR, F_BOOST, F_LOCKPCT, F_TEMPS, F_SUPPLY, F_SLIP,
  F_FLAGS0, F_FLAGS1, F_FLAGS2, F_FLAGS3,
  F_HRPM, F_HTHR, F_HCOOL, F_HLOCKPCT, F_HAGE,
  F_DMODE, F_DCAN, F_DBRAKE, F_DBRKCAN, F_DSTEER, F_DTC, F_DAWD, F_DAGE,
  F_WIFI, F_IP, F_HOST, F_POLL, F_SOURCE, F_HEAP,
  F_COUNT
};

lv_obj_t *g_val[F_COUNT] = {};

struct RowDef {
  uint8_t field;
  const char *name;
  bool flags;  // full-width wrapped block instead of a value column
};

const RowDef kRowsA[] = {
    {F_HDR, "magic/type/proto", false},
    {F_FW, "firmware", false},
    {F_GEN, "generation", false},
    {F_MODE, "mode / state", false},
    {F_NAME, "name", false},
    {F_SRC, "source MAC", false},
    {F_CHRSSI, "channel / rssi", false},
    {F_PKTS, "packets / rate", false},
    {F_SEQNONCE, "seq / nonce", false},
    {F_REJDUP, "rej / dup / gap", false},
    {F_SENDERS, "senders / age", false},
};

// What the car can feed, and the shape of the dashboard that follows from
// it. Detected vs table says how each answer was arrived at: the packet marks
// a field it has no value for (detected), except boost, which the controller
// has no sentinel for and which is therefore decided by generation.
const RowDef kRowsCaps[] = {
    {F_CAP_GEN, "generation", false},
    {F_CAP_BOOST, "boost source", false},
    {F_CAP_SUPPLY, "supply voltage", false},
    {F_CAP_TEMPS, "haldex temps", false},
    {F_CAP_STEER, "steering (packet)", false},
    {F_CAP_LAYOUT, "tile band", false},
};

// The CAN relay's health: enough to tell a bad radio link from a sender that
// cannot keep up. The frames themselves are on the CAN DATA page.
const RowDef kRowsRelay[] = {
    {F_RELAY_RATE, "frames/s / ids", false},
    {F_RELAY_AIR, "air lost pkt / %", false},
    {F_RELAY_SRC, "sender lost / age", false},
};

const RowDef kRowsB[] = {
    {F_RPM, "rpm", false},
    {F_SPEED, "speed", false},
    {F_THR, "throttle", false},
    {F_BOOST, "boost mbar", false},
    {F_LOCKPCT, "lock req / act", false},
    {F_TEMPS, "temps C c/o/m", false},
    {F_SUPPLY, "supply / steer", false},
    {F_SLIP, "slip F/R L/R", false},
    {F_FLAGS0, "flags[0]", true},
    {F_FLAGS1, "flags[1]", true},
    {F_FLAGS2, "flags[2]", true},
    {F_FLAGS3, "flags[3]", true},
};

const RowDef kRowsC[] = {
    {F_HRPM, "rpm / speed", false},
    {F_HTHR, "throttle / boost", false},
    {F_HCOOL, "coolant / oil", false},
    {F_HLOCKPCT, "lock req / act", false},
    {F_HAGE, "age ms", false},
    {F_DMODE, "mode / state", false},
    {F_DCAN, "chassis / haldex", false},
    {F_DBRAKE, "brakeIn / handbrIn", false},
    {F_DBRKCAN, "brakeCAN / handCAN", false},
    {F_DSTEER, "steerAngle", false},
    {F_DTC, "tc / asr", false},
    {F_DAWD, "awd/coup/tempPr", false},
    {F_DAGE, "age ms", false},
    {F_WIFI, "wifi / rssi", false},
    {F_IP, "ip", false},
    {F_HOST, "host", false},
    {F_POLL, "poll live / dash", false},
    {F_SOURCE, "source / pkt age", false},
    {F_HEAP, "heap / psram", false},
};

struct Section {
  const char *head;
  const RowDef *rows;
  int count;
};

struct Column {
  const char *head;
  const Section sections[3];
  int sectionCount;
};

const Column kColumns[3] = {
    {"ESP-NOW OHX_STATUS",
     {{nullptr, kRowsA, 11}, {"CAR CAPABILITY", kRowsCaps, 6}, {"CAN RELAY", kRowsRelay, 3}}, 3},
    {"PACKET VALUES (10 Hz)", {{nullptr, kRowsB, 8}, {"FLAGS (set bits)", kRowsB + 8, 4}, {nullptr, nullptr, 0}}, 2},
    {"HTTP / DASH", {{"HTTP /api/live", kRowsC, 5}, {"HTTP /api/dashboard", kRowsC + 5, 8}, {"LINK", kRowsC + 13, 6}}, 3},
};

// --------------------------------------------------------------- widgets
void setText(lv_obj_t *l, const char *text) {
  const char *cur = lv_label_get_text(l);
  if (!cur || strcmp(cur, text) != 0) lv_label_set_text(l, text);
}

void setTextColor(lv_obj_t *l, lv_color_t c) {
  // Setting a style always invalidates the object even when the value is
  // identical, so every style write on this page is guarded the same way the
  // dashboard guards its own.
  const lv_color_t cur = lv_obj_get_style_text_color(l, 0);
  if (cur.red != c.red || cur.green != c.green || cur.blue != c.blue) {
    lv_obj_set_style_text_color(l, c, 0);
  }
}

lv_obj_t *mkLabel(lv_obj_t *parent, int x, int y, int w, int h, const lv_font_t *font,
                  lv_color_t color, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_remove_flag(l, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_obj_set_style_pad_all(l, 0, 0);
  lv_label_set_text(l, text);
  lv_obj_set_size(l, w, h);
  lv_obj_set_pos(l, x, y);
  lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  return l;
}

lv_obj_t *mkPanel(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, COL_PANEL, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_color(o, COL_LINE, 0);
  lv_obj_set_style_radius(o, 4, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  return o;
}

lv_obj_t *mkButton(lv_obj_t *parent, int x, int y, int w, int h, const char *text,
                   lv_event_cb_t cb, lv_obj_t **outLabel) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, 4, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x12161b), 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, COL_LAMP_EDGE, 0);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t *l = mkLabel(b, 0, 0, w, h, FONT_CAP, COL_FG, text);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  if (outLabel) *outLabel = l;
  return b;
}

// -- value formatting. "--" is the sentinel for "the C6 has no value here",
// and it is why this page can be trusted: nothing absent is drawn as zero.
struct Buf {
  char s[28];
};

const char *fnum(Buf &b, float v, int dec) {
  if (isnan(v)) {
    snprintf(b.s, sizeof b.s, "--");
  } else if (dec <= 0) {
    snprintf(b.s, sizeof b.s, "%.0f", v);
  } else {
    snprintf(b.s, sizeof b.s, "%.*f", dec, v);
  }
  return b.s;
}

const char *fint(Buf &b, int v) {
  if (v < 0) {
    snprintf(b.s, sizeof b.s, "--");
  } else {
    snprintf(b.s, sizeof b.s, "%d", v);
  }
  return b.s;
}

const char *fbit(Buf &b, bool v) {
  snprintf(b.s, sizeof b.s, "%s", v ? "1" : "0");
  return b.s;
}

// Signed and unsigned forms with no "--" sentinel: RSSI is negative and the
// nonce is unsigned, and fint() would report both as "no value".
const char *fsint(Buf &b, int v) {
  snprintf(b.s, sizeof b.s, "%d", v);
  return b.s;
}

const char *fuint(Buf &b, uint32_t v) {
  snprintf(b.s, sizeof b.s, "%lu", (unsigned long)v);
  return b.s;
}

const char *fage(Buf &b, uint32_t ms) {
  if (ms == UINT32_MAX) {
    snprintf(b.s, sizeof b.s, "never");
  } else {
    snprintf(b.s, sizeof b.s, "%lu", (unsigned long)ms);
  }
  return b.s;
}

// "left / right", each side already formatted so an absent one shows "--".
void pair(lv_obj_t *dst, const char *left, const char *right) {
  char s[64];
  snprintf(s, sizeof s, "%s / %s", left, right);
  setText(dst, s);
}

// ------------------------------------------------------------ flag lists
struct FlagName {
  bool on;
  const char *nm;
};

void joinFlags(char *buf, size_t n, const FlagName *f, int count) {
  buf[0] = '\0';
  size_t used = 0;
  for (int i = 0; i < count; i++) {
    if (!f[i].on) continue;
    const int w = snprintf(buf + used, n - used, "%s%s", used ? " " : "", f[i].nm);
    if (w <= 0 || (size_t)w >= n - used) break;
    used += (size_t)w;
  }
  if (!used) snprintf(buf, n, "(none)");
}

// ------------------------------------------------------------- building
void onHold(lv_event_t *);
void onClose(lv_event_t *);

void addSection(lv_obj_t *panel, int &y, const Section &sec) {
  if (sec.head) {
    lv_obj_t *h = mkLabel(panel, kPadX, y, kColW - 2 * kPadX, kHeadH - 4, FONT_CAP, COL_AMBER,
                          sec.head);
    (void)h;
    y += kHeadH;
  }
  for (int i = 0; i < sec.count; i++) {
    const RowDef &r = sec.rows[i];
    if (r.flags) {
      mkLabel(panel, kPadX, y, kColW - 2 * kPadX, 16, FONT_CAP, COL_DIM, r.name);
      lv_obj_t *v = mkLabel(panel, kPadX, y + 16, kColW - 2 * kPadX, kFlagH - 16, FONT_CAP,
                            COL_GREEN, "(none)");
      lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
      g_val[r.field] = v;
      y += kFlagH;
    } else {
      mkLabel(panel, kPadX, y + 2, kNameW, kRowH - 2, FONT_CAP, COL_DIM, r.name);
      lv_obj_t *v = mkLabel(panel, kPadX + kNameW, y, kValueW, kRowH, FONT_SMALL, COL_FG, "--");
      lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
      g_val[r.field] = v;
      y += kRowH;
    }
  }
  y += 2;
}

void buildPage() {
  if (!lv_screen_active()) return;

  g_root = mkPanel(lv_layer_top(), 0, 0, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(g_root, COL_BG, 0);
  lv_obj_set_style_border_width(g_root, 0, 0);
  lv_obj_set_style_radius(g_root, 0, 0);
  lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);

  mkLabel(g_root, 8, 6, 300, 24, FONT_BODY, COL_AMBER, "RAW TELEMETRY " OHX_DASH_VERSION);
  g_title = mkLabel(g_root, 210, 10, 420, 18, FONT_CAP, COL_DIM, "waiting for data");
  // FREEZE holds the last reading on the glass so a moving value can be
  // photographed instead of guessed at.
  mkButton(g_root, 640, 5, 76, 26, "FREEZE", onHold, &g_holdLabel);
  mkButton(g_root, 722, 5, 70, 26, "CLOSE", onClose, nullptr);

  for (int c = 0; c < 3; c++) {
    const Column &col = kColumns[c];
    lv_obj_t *panel = mkPanel(g_root, kColX[c], kPanelY, kColW, kPanelH);
    mkLabel(panel, kPadX, 4, kColW - 2 * kPadX, 16, FONT_CAP, COL_WHITE, col.head);
    int y = 22;
    for (int s = 0; s < col.sectionCount; s++) addSection(panel, y, col.sections[s]);
  }
}

// ------------------------------------------------------------- updating
void updateTitle() {
  char t[96];
  char ageTxt[24];
  Buf age;
  const uint32_t ms = feed::nowAgeMs();
  if (ms == UINT32_MAX) {
    snprintf(ageTxt, sizeof ageTxt, "no packet");
  } else {
    snprintf(ageTxt, sizeof ageTxt, "%s ms", fage(age, ms));
  }
  const bool fresh = feed::fresh(cfg::get().staleMs);
  snprintf(t, sizeof t, "src %s | packet %s | %s%s", feed::sourceName(), ageTxt,
           fresh ? "LIVE" : "STALE", g_hold ? " | FROZEN" : "");
  setText(g_title, t);
  setTextColor(g_title, g_hold ? COL_AMBER : (fresh ? COL_GREEN : COL_RED));
  if (g_holdLabel) setText(g_holdLabel, g_hold ? "RESUME" : "FREEZE");
}

void refresh() {
  Buf a, b, c;
  char s[96];
  const Status p = ohxnow::last();
  const ohxnow::Stats n = ohxnow::stats();
  const telemetry::Live lv = feed::live();
  const telemetry::Dash dd = feed::dash();
  const telemetry::Stats ts = telemetry::stats();
  const cfg::Data &cfgNow = cfg::get();

  updateTitle();

  // ---- column A: the packet's frame
  setText(g_val[F_HDR], "--");
  if (p.valid) {
    snprintf(s, sizeof s, "%c%c t%u p%u", OHX_MAGIC0, OHX_MAGIC1, (unsigned)OHX_T_STATUS,
             (unsigned)OHX_PROTO);
    setText(g_val[F_HDR], s);
    snprintf(s, sizeof s, "%u.%u.%u", (unsigned)p.fw[0], (unsigned)p.fw[1], (unsigned)p.fw[2]);
    setText(g_val[F_FW], s);
    setText(g_val[F_GEN], fint(a, p.generation));
    pair(g_val[F_MODE], fint(a, p.mode), fint(b, p.haldexState));
    setText(g_val[F_NAME], p.name[0] ? p.name : "--");
    snprintf(s, sizeof s, "%02X:%02X:%02X:%02X:%02X:%02X", p.src[0], p.src[1], p.src[2], p.src[3],
             p.src[4], p.src[5]);
    setText(g_val[F_SRC], s);
  } else {
    const char *none = "--";
    for (uint8_t i = F_FW; i <= F_SRC; i++) setText(g_val[i], none);
  }
  pair(g_val[F_CHRSSI], fsint(a, (int)n.channel), fsint(b, (int)n.rssi));
  snprintf(s, sizeof s, "%lu / %.1f Hz", (unsigned long)n.packets, (double)n.hz);
  setText(g_val[F_PKTS], s);
  pair(g_val[F_SEQNONCE], fint(a, (int)p.seq), fuint(b, p.nonce));
  snprintf(s, sizeof s, "%lu / %lu / %lu", (unsigned long)n.rejects, (unsigned long)n.duplicates,
           (unsigned long)n.gaps);
  setText(g_val[F_REJDUP], s);
  snprintf(s, sizeof s, "%lu / %s", (unsigned long)n.senders, fage(b, n.ageMs));
  setText(g_val[F_SENDERS], s);

  // ---- what the car can fill, and the dashboard's shape. "waiting" is not
  // the same as "no": until the decision settles, nothing has been ruled out.
  const cap::Caps &cp = cap::get();
  setText(g_val[F_CAP_GEN], cp.genKnown ? fint(a, cp.generation) : "unknown");
  const char *waiting = cp.settled ? nullptr : "waiting";
  setText(g_val[F_CAP_BOOST], waiting ? waiting : (cp.boost ? "yes (table)" : "no (table)"));
  setText(g_val[F_CAP_SUPPLY],
          waiting ? waiting : (cp.supplyVoltage ? "yes (detected)" : "no (detected)"));
  setText(g_val[F_CAP_TEMPS],
          waiting ? waiting : (cp.haldexTemps ? "yes (detected)" : "no (detected)"));
  setText(g_val[F_CAP_STEER],
          waiting ? waiting : (cp.steering ? "yes (detected)" : "no (detected)"));
  setText(g_val[F_CAP_LAYOUT], layout::bandFor(cp.variant).name);

  // ---- the CAN relay
  {
    const canrelay::Stats rs = canrelay::stats();
    const uint32_t total = rs.packets + rs.airLost;
    float lostPercent = 0.0f;
    if (total > 0) lostPercent = (float)rs.airLost * 100.0f / (float)total;
    snprintf(s, sizeof s, "%lu / %d", (unsigned long)rs.perSec, rs.ids);
    setText(g_val[F_RELAY_RATE], s);
    snprintf(s, sizeof s, "%lu / %.1f", (unsigned long)rs.airLost, (double)lostPercent);
    setText(g_val[F_RELAY_AIR], s);
    snprintf(s, sizeof s, "%lu / %s", (unsigned long)rs.srcLost, fage(a, rs.ageMs));
    setText(g_val[F_RELAY_SRC], s);
  }

  // ---- column B: the packet's values
  if (p.valid) {
    setText(g_val[F_RPM], fnum(a, p.rpm, 0));
    setText(g_val[F_SPEED], fnum(a, p.speed, 0));
    setText(g_val[F_THR], fnum(a, p.throttle, 1));
    setText(g_val[F_BOOST], fnum(a, p.boost, 0));
    pair(g_val[F_LOCKPCT], fnum(a, p.lockTarget, 0), fnum(b, p.lockActual, 0));
    snprintf(s, sizeof s, "%s/%s/%s", fnum(a, p.clutchTemp, 0), fnum(b, p.oilTemp, 0),
             fnum(c, p.moduleTemp, 0));
    setText(g_val[F_TEMPS], s);
    snprintf(s, sizeof s, "%s / %s", fnum(a, p.supplyV, 1), fnum(b, p.steering, 1));
    setText(g_val[F_SUPPLY], s);
    snprintf(s, sizeof s, "%s/%s %s/%s", fnum(a, p.slip[0], 0), fnum(b, p.slip[1], 0),
             fnum(a, p.slip[2], 0), fnum(b, p.slip[3], 0));
    setText(g_val[F_SLIP], s);
  } else {
    for (uint8_t i = F_RPM; i <= F_SLIP; i++) setText(g_val[i], "--");
  }

  // ---- column B: the flags, listed by name so nothing has to be decoded
  // by hand from the C6 source at the roadside
  FlagName f0[] = {{p.standalone, "STANDALONE"}, {p.chassisCan, "CHASSIS_CAN"},
                   {p.haldexCan, "HALDEX_CAN"},   {p.ctrlOff, "CTRL_OFF"},
                   {p.busFail, "BUS_FAIL"},       {p.modeOverride, "MODE_OVERRIDE"},
                   {p.canBcast, "CAN_BCAST"},     {p.learned, "LEARNED"}};
  FlagName f1[] = {{p.tcForce, "TC_FORCE"},       {p.hazard, "HAZARD"},
                   {p.extBtn, "EXT_BTN"},         {p.brake, "BRAKE"},
                   {p.handbrake, "HANDBRAKE"},    {p.tempProtection, "TEMP_PROT"},
                   {p.couplingOpen, "COUPLING_OPEN"}, {p.speedLimit, "SPEED_LIMIT"}};
  FlagName f2[] = {{p.learn, "LEARN"},         {p.longLearn, "LONG_LEARN"},
                   {p.ctrlAccept, "CTRL_ACCEPT"}, {p.keyed, "KEY"},
                   {p.fixHunting, "FIX_HUNT"},  {p.steerScale, "STEER_SCALE"},
                   {p.analyzer, "ANALYZER"},    {p.liveDiag, "LIVE_DIAG"}};
  FlagName f3[] = {{p.asrOff, "ASR_OFF"}};
  if (p.valid) {
    joinFlags(s, sizeof s, f0, 8);
    setText(g_val[F_FLAGS0], s);
    joinFlags(s, sizeof s, f1, 8);
    setText(g_val[F_FLAGS1], s);
    joinFlags(s, sizeof s, f2, 8);
    setText(g_val[F_FLAGS2], s);
    joinFlags(s, sizeof s, f3, 1);
    setText(g_val[F_FLAGS3], s);
  } else {
    for (uint8_t i = F_FLAGS0; i <= F_FLAGS3; i++) setText(g_val[i], "no packet yet");
  }

  // ---- column C: the HTTP fallback, and the link itself
  pair(g_val[F_HRPM], fnum(a, lv.rpm, 0), fnum(b, lv.speed, 0));
  pair(g_val[F_HTHR], fnum(a, lv.throttle, 0), fnum(b, lv.boost, 0));
  pair(g_val[F_HCOOL], fnum(a, lv.coolant, 0), fnum(b, lv.oil, 0));
  pair(g_val[F_HLOCKPCT], fnum(a, lv.lockTarget, 0), fnum(b, lv.lockActual, 0));
  setText(g_val[F_HAGE], fage(a, lv.ageMs));

  pair(g_val[F_DMODE], fint(a, dd.mode), fint(b, dd.haldexState));
  pair(g_val[F_DCAN], fbit(a, dd.chassisCAN), fbit(b, dd.haldexCAN));
  // brakeIn / handbrakeIn are the wired inputs; brakeCAN / handCAN are what
  // the chassis CAN says. The lamps on the dashboard are fed by the first
  // pair only, which is the working theory for why they never light.
  pair(g_val[F_DBRAKE], fbit(a, dd.brakeIn), fbit(b, dd.handbrakeIn));
  pair(g_val[F_DBRKCAN], fint(a, dd.brakeFromCAN), fint(b, dd.handbrakeFromCAN));
  // Gen1 gating: the packet carries a steering angle, the JSON path reports
  // this one as null. Seeing them disagree is the point of the row.
  setText(g_val[F_DSTEER], fnum(a, dd.steeringAngle, 0));
  pair(g_val[F_DTC], fbit(a, dd.tcOn), fbit(b, dd.asrOn));
  snprintf(s, sizeof s, "%s/%s/%s", fbit(a, dd.awdWarning), fbit(b, dd.couplingOpen),
           fbit(c, dd.tempProtection));
  setText(g_val[F_DAWD], s);
  setText(g_val[F_DAGE], fage(a, dd.ageMs));

  pair(g_val[F_WIFI], ts.wifiUp ? "up" : "DOWN", fsint(b, (int)ts.rssi));
  snprintf(s, sizeof s, "%u.%u.%u.%u", (ts.ip >> 0) & 0xFF, (ts.ip >> 8) & 0xFF,
           (ts.ip >> 16) & 0xFF, (ts.ip >> 24) & 0xFF);
  setText(g_val[F_IP], s);
  setText(g_val[F_HOST], cfgNow.host);
  snprintf(s, sizeof s, "%u / %u ms", (unsigned)cfgNow.pollLiveMs, (unsigned)cfgNow.pollDashMs);
  setText(g_val[F_POLL], s);
  snprintf(s, sizeof s, "%s / %s", feed::sourceName(), fage(a, feed::nowAgeMs()));
  setText(g_val[F_SOURCE], s);
  snprintf(s, sizeof s, "%uk / %uk", (unsigned)(ESP.getFreeHeap() / 1024),
           (unsigned)(ESP.getFreePsram() / 1024));
  setText(g_val[F_HEAP], s);
}

// --------------------------------------------------------------- report
// The same content as text, in the order it is drawn. If a laptop is
// attached, this is the capture: the page's own numbers, ready to paste back
// instead of being read off a photograph.
void dumpToSerial() {
  Serial.println("[raw] --- raw telemetry snapshot ---");
  for (int c = 0; c < 3; c++) {
    const Column &col = kColumns[c];
    Serial.printf("[raw] %s\n", col.head);
    for (int s = 0; s < col.sectionCount; s++) {
      const Section &sec = col.sections[s];
      if (sec.head) Serial.printf("[raw]   %s\n", sec.head);
      for (int i = 0; i < sec.count; i++) {
        const RowDef &r = sec.rows[i];
        const char *t = g_val[r.field] ? lv_label_get_text(g_val[r.field]) : "?";
        Serial.printf("[raw]   %-18s %s\n", r.name, t);
      }
    }
  }
  Serial.println("[raw] --- end snapshot ---");
}

// -------------------------------------------------------------- events
void onHold(lv_event_t *) {
  g_hold = !g_hold;
  updateTitle();
  // Freezing is the moment someone wants a record, so take one.
  if (g_hold) dumpToSerial();
  Serial.printf("[raw] %s\n", g_hold ? "frozen" : "live");
}

void onClose(lv_event_t *) { show(false); }

}  // namespace

// ---------------------------------------------------------------- API
void begin() { buildPage(); }

void tick() {
  if (!g_visible || g_hold || !g_root) return;
  const uint32_t now = millis();
  if ((uint32_t)(now - g_lastMs) < kRefreshMs) return;
  g_lastMs = now;
  refresh();
}

void show(bool visible) {
  if (!g_root) {
    g_visible = visible;
    return;
  }
  g_visible = visible;
  if (visible) {
    // setPage() hides the dashboard and closes settings in one move; the
    // dashboard is what must come off the glass, not just be covered.
    ui::setPage(ui::Page::Raw);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    g_lastMs = 0;
    refresh();       // fill it now: an empty page for one frame looks like a fault
    dumpToSerial();  // and put it on the console, for whoever has a laptop
  } else {
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    if (ui::page() == ui::Page::Raw) ui::setPage(ui::Page::Dashboard);
  }
  Serial.printf("[raw] page %s\n", visible ? "open" : "closed");
}

void toggle() { show(!g_visible); }

bool visible() { return g_visible; }

}  // namespace rawui
