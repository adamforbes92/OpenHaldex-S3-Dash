#!/usr/bin/env python3
"""One-off patch: cut needless invalidation in the tile readouts.

Second round of measurements: readouts still cost 29 ms/frame because
styles were being re-applied (and therefore invalidated) every single
frame even when nothing about them had changed, and because the big value
labels were boxed far wider than the glyphs they hold.
"""
import sys

P = "src/ui/dash_ui.cpp"
s = open(P).read()
n = 0


def sub(old, new):
    global s, n
    if old not in s:
        print("MISS:", old.strip().splitlines()[0][:70])
        return
    s = s.replace(old, new, 1)
    n += 1


# 1 ------------- colour setters that do nothing when nothing changed -----
sub(
    """void setTextColor(lv_obj_t *label, lv_color_t c) { lv_obj_set_style_text_color(label, c, 0); }""",
    """bool sameColor(lv_color_t a, lv_color_t b) {
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
}""",
)

# 2 ------------------------------ lamps only when their state changes ----
sub(
    """void lampState(lv_obj_t *l, bool on, bool red) {
  lv_color_t bg = red ? COL_RED : COL_AMBER;
  lv_obj_set_style_bg_color(l, on ? bg : COL_LAMP_OFF, 0);
  lv_obj_set_style_border_color(l, on ? bg : COL_LAMP_EDGE, 0);
  lv_obj_set_style_text_color(l, on ? (red ? lv_color_white() : lv_color_black())
                                    : COL_LAMP_TXT_OFF,
                              0);
}""",
    """// Called every frame for five lamps; keep the previous state so an
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
}""",
)

# 3 ------------------------------------------------ chevron: tach handle --
sub(
    """    lv_obj_set_style_bg_color(w.shiftLamp, flash ? COL_RED : lv_color_hex(0x2a0f0f), 0);
    lv_obj_set_style_border_color(w.shiftLamp, flash ? lv_color_white() : lv_color_hex(0x481a1a),
                                  0);
    lv_obj_set_style_text_color(w.shiftLamp, flash ? lv_color_white() : lv_color_hex(0x3a1414),
                                0);""",
    """    setBgColor(w.shiftLamp, flash ? COL_RED : lv_color_hex(0x2a0f0f), 0);
    setBorderColor(w.shiftLamp, flash ? lv_color_white() : lv_color_hex(0x481a1a));
    setTextColor(w.shiftLamp, flash ? lv_color_white() : lv_color_hex(0x3a1414));""",
)

# 4 --------------------------- bar fills only when the colour changes ----
sub(
    """    setBar(w.boostBar, pct);
    lv_obj_set_style_bg_color(w.boostBar, bcol, LV_PART_INDICATOR);""",
    """    setBar(w.boostBar, pct);
    setBgColor(w.boostBar, bcol, LV_PART_INDICATOR);""",
)
sub(
    """    setBar(w.coolBar, pct);
    lv_obj_set_style_bg_color(w.coolBar, colFor(v.coolant, c.coolWarn, c.coolCrit),
                              LV_PART_INDICATOR);""",
    """    setBar(w.coolBar, pct);
    setBgColor(w.coolBar, colFor(v.coolant, c.coolWarn, c.coolCrit), LV_PART_INDICATOR);""",
)
sub(
    """    setBar(w.oilBar, pct);
    lv_obj_set_style_bg_color(w.oilBar, colFor(v.oil, c.oilWarn, c.oilCrit), LV_PART_INDICATOR);""",
    """    setBar(w.oilBar, pct);
    setBgColor(w.oilBar, colFor(v.oil, c.oilWarn, c.oilCrit), LV_PART_INDICATOR);""",
)

# 5 --------------------------------------------- connection dot / text ----
sub(
    """  if (!s.wifiUp) {
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
    lv_obj_remove_flag(w.stale, LV_OBJ_FLAG_HIDDEN);""",
    """  if (!s.wifiUp) {
    setText(w.connTxt, "NO WIFI");
    setTextColor(w.connTxt, COL_RED);
    setBgColor(w.dot, COL_RED, 0);
  } else if (stale) {
    setText(w.connTxt, "STALE");
    setTextColor(w.connTxt, COL_AMBER);
    setBgColor(w.dot, COL_AMBER, 0);
  } else {
    setText(w.connTxt, "LIVE");
    setTextColor(w.connTxt, COL_GREEN);
    setBgColor(w.dot, COL_GREEN, 0);
  }

  if (stale) {
    lv_obj_remove_flag(w.stale, LV_OBJ_FLAG_HIDDEN);""",
)

# 6 --- tighten the wide value boxes: LVGL repaints the whole label box ----
sub(
    """  w.rpmDigits = boxedLabel(w.tach, L::TACH_CX, L::TACH_CY - (int)(L::TACH_R * 0.30f), 320, 60,
                           FONT_TACH, COL_FG, "--");""",
    """  // The box is what gets repainted, so keep it just wider than "8000".
  w.rpmDigits = boxedLabel(w.tach, L::TACH_CX, L::TACH_CY - (int)(L::TACH_R * 0.30f), 210, 60,
                           FONT_TACH, COL_FG, "--");""",
)
sub(
    """  w.speedVal = fixedLabel(t0, L::TILE_PAD_H - 2, 44, L::COL0_W - 2 * L::TILE_PAD_H, 58,
                          FONT_BIG, COL_FG, "--");""",
    """  w.speedVal = fixedLabel(t0, L::TILE_PAD_H - 2, 44, 132, 58, FONT_BIG, COL_FG, "--");""",
)
sub(
    """  w.boostVal = fixedLabel(t1, L::TILE_PAD_H - 2, 44, L::COL1_W - 2 * L::TILE_PAD_H, 58,
                          FONT_BIG, COL_FG, "--");""",
    """  w.boostVal = fixedLabel(t1, L::TILE_PAD_H - 2, 44, 140, 58, FONT_BIG, COL_FG, "--");""",
)

# 7 ------------- tile readouts on the 10 Hz data cadence, not the 30 Hz --
sub(
    """  uint32_t t0 = micros();
  updateTach(c, g_shown.rpm, stale);
  uint32_t t1 = micros();
  if (dbg(16)) updateReadouts(c, l, d, stale);""",
    """  uint32_t t0 = micros();
  updateTach(c, g_shown.rpm, stale);
  uint32_t t1 = micros();
  // The tiles carry values that only arrive at 10 Hz and are already eased,
  // so repainting them 30 times a second buys nothing. The tach above stays
  // at the full rate because the needle is what the eye tracks.
  static uint32_t lastReadouts = 0;
  if (dbg(16) && (uint32_t)(now - lastReadouts) >= 100) {
    lastReadouts = now;
    updateReadouts(c, l, d, stale);
  }""",
)

open(P, "w").write(s)
print("applied %d edits" % n)
if n < 14:
    sys.exit(1)
