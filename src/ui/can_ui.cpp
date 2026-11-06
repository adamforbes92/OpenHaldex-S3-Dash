// =====================================================================
// can_ui.cpp - the CAN DATA page
//
// Left: every id heard, sorted, one row each. Tap a row to select it.
// Right: the selected frame's signals, decoded from the DBC tables.
//
// Built the way the raw page is, for the same reasons: it sits on lv_layer_top()
// with the dashboard hidden rather than covered, and every row is a fixed-size
// clipped label, so a changing value repaints one row and not the whole panel.
// Long lists are paged rather than scrolled, because scrolling repaints everything
// it moves and this panel has no bandwidth to spare.
// =====================================================================
#include "ui/can_ui.h"

#include <Arduino.h>
#include <lvgl.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "net/can_relay.h"
#include "net/dbc.h"
#include "net/ohx_now.h"
#include "theme.h"
#include "ui/dash_ui.h"

namespace canui {
namespace {

// ------------------------------------------------------------- geometry
constexpr int kPanelY = 36;
constexpr int kPanelH = LCD_V_RES - kPanelY - 6;
constexpr int kListX = 6;
constexpr int kListW = 300;
constexpr int kDetailX = 312;
constexpr int kDetailW = 482;
constexpr int kPadX = 6;
constexpr int kRows = 19;
constexpr int kRowH = 20;
constexpr int kRowsY = 22;
constexpr int kPagerY = kRowsY + kRows * kRowH + 4;

// Nobody reads a number that changes more than a few times a second, and each
// change is a repaint.
constexpr uint32_t kRefreshMs = 200;
// Greyed out after this long: heard once, no longer on the bus.
constexpr uint32_t kQuietMs = 2000;
constexpr uint32_t kExtendedFlag = 0x80000000;

lv_obj_t *g_root = nullptr;
lv_obj_t *g_title = nullptr;
lv_obj_t *g_holdLabel = nullptr;
bool g_visible = false;
bool g_hold = false;
uint32_t g_lastMs = 0;

// The list panel: one row per id.
lv_obj_t *g_listHead = nullptr;
lv_obj_t *g_listRow[kRows] = {};
lv_obj_t *g_listName[kRows] = {};
lv_obj_t *g_listRate[kRows] = {};
lv_obj_t *g_listPage = nullptr;

// The detail panel: one row per signal.
lv_obj_t *g_detailHead = nullptr;
lv_obj_t *g_detailName[kRows] = {};
lv_obj_t *g_detailValue[kRows] = {};
lv_obj_t *g_detailUnit[kRows] = {};
lv_obj_t *g_detailPage = nullptr;

// Every id heard, sorted. An extended id has kExtendedFlag set, which also sorts
// them after the 11-bit ones. Only rebuilt when a new id turns up.
uint32_t g_ids[canrelay::kMaxFrames];
int g_idCount = 0;

uint32_t g_selected = 0;
bool g_haveSelection = false;
int g_listPageIndex = 0;
int g_detailPageIndex = 0;

// --------------------------------------------------------------- widgets
void setText(lv_obj_t *l, const char *text) {
  const char *cur = lv_label_get_text(l);
  if (!cur || strcmp(cur, text) != 0) lv_label_set_text(l, text);
}

void setTextColor(lv_obj_t *l, lv_color_t c) {
  // Setting a style always invalidates the object even when the value is
  // identical, so every style write here is guarded, as on the other pages.
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
  lv_obj_t *l = mkLabel(b, 0, 6, w, h - 6, FONT_CAP, COL_FG, text);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  if (outLabel) *outLabel = l;
  return b;
}

// -------------------------------------------------------------- the data
// The DBC table for the car the controller says it is in, or nullptr.
const dbc::Table *currentTable() {
  const ohxnow::Status status = ohxnow::last();
  if (!status.valid) return nullptr;
  return dbc::forGeneration(status.generation);
}

// Take in any new ids, keeping the list sorted. Only ever adds: the relay never
// forgets an id, so this runs a handful of times just after boot and then stops.
void refreshIdList() {
  const int heard = canrelay::count();
  if (heard == g_idCount) return;
  for (int i = g_idCount; i < heard && g_idCount < canrelay::kMaxFrames; i++) {
    canrelay::Frame frame;
    if (!canrelay::frameAt(i, frame)) break;
    uint32_t key = frame.id;
    if (frame.extended) key = key | kExtendedFlag;
    int position = g_idCount;
    while (position > 0 && g_ids[position - 1] > key) {
      g_ids[position] = g_ids[position - 1];
      position--;
    }
    g_ids[position] = key;
    g_idCount++;
  }
  if (!g_haveSelection && g_idCount > 0) {
    g_selected = g_ids[0];
    g_haveSelection = true;
  }
}

int pageCount(int items) {
  if (items <= 0) return 1;
  return (items + kRows - 1) / kRows;
}

// ------------------------------------------------------------- updating
void updateTitle(const dbc::Table *table) {
  const canrelay::Stats s = canrelay::stats();
  char t[110];
  if (!canrelay::fresh()) {
    snprintf(t, sizeof t, "no relay packets: set Relay CAN Frames to Gauges on the controller");
    setText(g_title, t);
    setTextColor(g_title, COL_AMBER);
  } else {
    const uint32_t total = s.packets + s.airLost;
    float lostPercent = 0.0f;
    if (total > 0) lostPercent = (float)s.airLost * 100.0f / (float)total;
    snprintf(t, sizeof t, "%lu f/s | %d ids | air lost %.1f%% | sender lost %lu | %s%s",
             (unsigned long)s.perSec, s.ids, (double)lostPercent, (unsigned long)s.srcLost,
             table ? table->name : "no DBC", g_hold ? " | FROZEN" : "");
    setText(g_title, t);
    setTextColor(g_title, g_hold ? COL_AMBER : COL_GREEN);
  }
  if (g_holdLabel) setText(g_holdLabel, g_hold ? "RESUME" : "FREEZE");
}

void updateList(const dbc::Table *table) {
  char s[64];
  snprintf(s, sizeof s, "ON THE BUS (%d)", g_idCount);
  setText(g_listHead, s);

  const int pages = pageCount(g_idCount);
  if (g_listPageIndex >= pages) g_listPageIndex = pages - 1;
  snprintf(s, sizeof s, "%d / %d", g_listPageIndex + 1, pages);
  setText(g_listPage, s);

  for (int row = 0; row < kRows; row++) {
    const int index = g_listPageIndex * kRows + row;
    if (index >= g_idCount) {
      setText(g_listName[row], "");
      setText(g_listRate[row], "");
      continue;
    }
    const uint32_t key = g_ids[index];
    const bool extended = (key & kExtendedFlag) != 0;
    const uint32_t id = key & ~kExtendedFlag;

    canrelay::Frame frame;
    if (!canrelay::find(id, extended, frame)) continue;

    const dbc::Message *message = nullptr;
    if (table) message = dbc::find(*table, id, extended);
    const char *name = "";
    if (message) name = dbc::name(*table, message->nameAt);
    if (extended) {
      snprintf(s, sizeof s, "%08lX %s", (unsigned long)id, name);
    } else {
      snprintf(s, sizeof s, "%03lX  %s", (unsigned long)id, name);
    }
    setText(g_listName[row], s);

    snprintf(s, sizeof s, "%u/s", (unsigned)frame.perSec);
    setText(g_listRate[row], s);

    lv_color_t colour = COL_FG;
    if (frame.ageMs > kQuietMs) colour = COL_DIM;
    if (g_haveSelection && key == g_selected) colour = COL_CYAN;
    setTextColor(g_listName[row], colour);
    setTextColor(g_listRate[row], colour);
  }
}

void clearDetailRows(int from) {
  for (int row = from; row < kRows; row++) {
    setText(g_detailName[row], "");
    setText(g_detailValue[row], "");
    setText(g_detailUnit[row], "");
  }
}

// A whole number when the signal only moves in whole steps, two places otherwise.
void formatValue(char *out, size_t size, const dbc::Table &table, const dbc::Signal &signal,
                 double value) {
  const dbc::Scale &scale = table.scales[signal.scaleAt];
  const bool whole = scale.factor == (float)(int32_t)scale.factor &&
                     scale.offset == (float)(int32_t)scale.offset;
  if (whole) {
    snprintf(out, size, "%.0f", value);
  } else {
    snprintf(out, size, "%.2f", value);
  }
}

void updateDetail(const dbc::Table *table) {
  char s[96];
  if (!g_haveSelection) {
    setText(g_detailHead, "nothing heard yet");
    setText(g_detailPage, "");
    clearDetailRows(0);
    return;
  }

  const bool extended = (g_selected & kExtendedFlag) != 0;
  const uint32_t id = g_selected & ~kExtendedFlag;
  canrelay::Frame frame;
  if (!canrelay::find(id, extended, frame)) return;

  // The header carries the raw bytes, so a frame the DBC does not know about, or
  // has wrong, can still be read.
  const dbc::Message *message = nullptr;
  if (table) message = dbc::find(*table, id, extended);
  char bytes[28] = "";
  size_t used = 0;
  for (uint8_t i = 0; i < frame.length && i < 8; i++) {
    used += snprintf(bytes + used, sizeof(bytes) - used, "%02X ", frame.data[i]);
  }
  const char *name = "not in the DBC";
  if (message) name = dbc::name(*table, message->nameAt);
  snprintf(s, sizeof s, "%lX %s | %s| %u/s", (unsigned long)id, name, bytes,
           (unsigned)frame.perSec);
  setText(g_detailHead, s);

  if (!message) {
    setText(g_detailPage, "");
    clearDetailRows(0);
    return;
  }

  // Count what is really in this frame first: a multiplexed signal only exists
  // when its selector says so, so the page count has to come from this frame.
  int active = 0;
  for (uint16_t i = 0; i < message->signalCount; i++) {
    const dbc::Signal &signal = table->signals[message->firstSignal + i];
    if (dbc::isActive(*table, *message, signal, frame.data, frame.length)) active++;
  }
  const int pages = pageCount(active);
  if (g_detailPageIndex >= pages) g_detailPageIndex = pages - 1;
  snprintf(s, sizeof s, "%d / %d", g_detailPageIndex + 1, pages);
  setText(g_detailPage, s);

  const int skip = g_detailPageIndex * kRows;
  int seen = 0;
  int row = 0;
  for (uint16_t i = 0; i < message->signalCount && row < kRows; i++) {
    const dbc::Signal &signal = table->signals[message->firstSignal + i];
    if (!dbc::isActive(*table, *message, signal, frame.data, frame.length)) continue;
    seen++;
    if (seen <= skip) continue;

    setText(g_detailName[row], dbc::name(*table, signal.nameAt));
    double value = 0.0;
    if (dbc::read(*table, signal, frame.data, frame.length, value)) {
      formatValue(s, sizeof s, *table, signal, value);
      setText(g_detailValue[row], s);
    } else {
      setText(g_detailValue[row], "--");  // the frame is shorter than the DBC says
    }
    setText(g_detailUnit[row], dbc::unit(*table, signal));
    row++;
  }
  clearDetailRows(row);
}

void refresh() {
  const dbc::Table *table = currentTable();
  refreshIdList();
  updateTitle(table);
  updateList(table);
  updateDetail(table);
}

// -------------------------------------------------------------- events
void onListRow(lv_event_t *e) {
  const int row = (int)(intptr_t)lv_event_get_user_data(e);
  const int index = g_listPageIndex * kRows + row;
  if (index >= g_idCount) return;
  g_selected = g_ids[index];
  g_haveSelection = true;
  g_detailPageIndex = 0;
  refresh();
}

void onListPrev(lv_event_t *) {
  if (g_listPageIndex > 0) g_listPageIndex--;
  refresh();
}

void onListNext(lv_event_t *) {
  if (g_listPageIndex + 1 < pageCount(g_idCount)) g_listPageIndex++;
  refresh();
}

void onDetailPrev(lv_event_t *) {
  if (g_detailPageIndex > 0) g_detailPageIndex--;
  refresh();
}

void onDetailNext(lv_event_t *) {
  g_detailPageIndex++;  // updateDetail() pulls it back if that is past the end
  refresh();
}

void onHold(lv_event_t *) {
  g_hold = !g_hold;
  updateTitle(currentTable());
  if (g_hold) dumpToSerial();  // freezing is the moment someone wants a record
}

void onClose(lv_event_t *) { show(false); }

// ------------------------------------------------------------- building
void buildPage() {
  if (!lv_screen_active()) return;

  g_root = mkPanel(lv_layer_top(), 0, 0, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(g_root, COL_BG, 0);
  lv_obj_set_style_border_width(g_root, 0, 0);
  lv_obj_set_style_radius(g_root, 0, 0);
  lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);

  mkLabel(g_root, 8, 6, 110, 24, FONT_BODY, COL_AMBER, "CAN DATA");
  g_title = mkLabel(g_root, 112, 10, 520, 18, FONT_CAP, COL_DIM, "waiting for data");
  mkButton(g_root, 640, 5, 76, 26, "FREEZE", onHold, &g_holdLabel);
  mkButton(g_root, 722, 5, 70, 26, "CLOSE", onClose, nullptr);

  // ---- the list
  lv_obj_t *list = mkPanel(g_root, kListX, kPanelY, kListW, kPanelH);
  g_listHead = mkLabel(list, kPadX, 4, kListW - 2 * kPadX, 16, FONT_CAP, COL_WHITE, "ON THE BUS");
  for (int row = 0; row < kRows; row++) {
    // The row itself takes the tap. Its labels are not clickable, so a tap on
    // either of them lands here, and it spans the panel so it is easy to hit.
    lv_obj_t *r = lv_obj_create(list);
    lv_obj_remove_style_all(r);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(r, 0, kRowsY + row * kRowH);
    lv_obj_set_size(r, kListW, kRowH);
    lv_obj_add_event_cb(r, onListRow, LV_EVENT_CLICKED, (void *)(intptr_t)row);
    g_listRow[row] = r;
    g_listName[row] = mkLabel(r, kPadX, 3, kListW - 2 * kPadX - 52, 16, FONT_CAP, COL_FG, "");
    g_listRate[row] = mkLabel(r, kListW - kPadX - 50, 3, 50, 16, FONT_CAP, COL_DIM, "");
    lv_obj_set_style_text_align(g_listRate[row], LV_TEXT_ALIGN_RIGHT, 0);
  }
  mkButton(list, kPadX, kPagerY, 76, 26, "PREV", onListPrev, nullptr);
  g_listPage = mkLabel(list, kPadX + 82, kPagerY + 6, kListW - 2 * (kPadX + 82), 16, FONT_CAP,
                       COL_DIM, "");
  lv_obj_set_style_text_align(g_listPage, LV_TEXT_ALIGN_CENTER, 0);
  mkButton(list, kListW - kPadX - 76, kPagerY, 76, 26, "NEXT", onListNext, nullptr);

  // ---- the selected frame's signals
  lv_obj_t *detail = mkPanel(g_root, kDetailX, kPanelY, kDetailW, kPanelH);
  g_detailHead = mkLabel(detail, kPadX, 4, kDetailW - 2 * kPadX, 16, FONT_CAP, COL_WHITE, "");
  for (int row = 0; row < kRows; row++) {
    const int y = kRowsY + row * kRowH;
    g_detailName[row] = mkLabel(detail, kPadX, y + 3, 290, 16, FONT_CAP, COL_DIM, "");
    g_detailValue[row] = mkLabel(detail, kPadX + 292, y + 1, 110, 18, FONT_SMALL, COL_FG, "");
    lv_obj_set_style_text_align(g_detailValue[row], LV_TEXT_ALIGN_RIGHT, 0);
    g_detailUnit[row] = mkLabel(detail, kPadX + 408, y + 3, 60, 16, FONT_CAP, COL_DIM, "");
  }
  mkButton(detail, kPadX, kPagerY, 76, 26, "PREV", onDetailPrev, nullptr);
  g_detailPage = mkLabel(detail, kPadX + 82, kPagerY + 6, kDetailW - 2 * (kPadX + 82), 16, FONT_CAP,
                         COL_DIM, "");
  lv_obj_set_style_text_align(g_detailPage, LV_TEXT_ALIGN_CENTER, 0);
  mkButton(detail, kDetailW - kPadX - 76, kPagerY, 76, 26, "NEXT", onDetailNext, nullptr);
}

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
  g_visible = visible;
  if (!g_root) return;
  if (visible) {
    // setPage() hides the dashboard and closes settings in one move; the
    // dashboard is what must come off the glass, not just be covered.
    ui::setPage(ui::Page::Can);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    g_lastMs = 0;
    refresh();
  } else {
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    if (ui::page() == ui::Page::Can) ui::setPage(ui::Page::Dashboard);
  }
  Serial.printf("[can] page %s\n", visible ? "open" : "closed");
}

void toggle() { show(!g_visible); }

bool visible() { return g_visible; }

void dumpToSerial() {
  refreshIdList();
  const dbc::Table *table = currentTable();
  const canrelay::Stats s = canrelay::stats();
  Serial.printf("[can] --- %d ids, %lu frames, air lost %lu pkt, sender lost %lu, DBC %s ---\n",
                s.ids, (unsigned long)s.frames, (unsigned long)s.airLost, (unsigned long)s.srcLost,
                table ? table->name : "none");
  for (int i = 0; i < g_idCount; i++) {
    const bool extended = (g_ids[i] & kExtendedFlag) != 0;
    const uint32_t id = g_ids[i] & ~kExtendedFlag;
    canrelay::Frame frame;
    if (!canrelay::find(id, extended, frame)) continue;
    const dbc::Message *message = nullptr;
    if (table) message = dbc::find(*table, id, extended);
    Serial.printf("[can] %8lX %5u/s  ", (unsigned long)id, (unsigned)frame.perSec);
    for (uint8_t b = 0; b < 8; b++) {
      if (b < frame.length) {
        Serial.printf("%02X ", frame.data[b]);
      } else {
        Serial.print("   ");
      }
    }
    Serial.println(message ? dbc::name(*table, message->nameAt) : "");
  }

  if (!g_haveSelection || !table) return;
  const bool extended = (g_selected & kExtendedFlag) != 0;
  const uint32_t id = g_selected & ~kExtendedFlag;
  const dbc::Message *message = dbc::find(*table, id, extended);
  canrelay::Frame frame;
  if (!message || !canrelay::find(id, extended, frame)) return;
  Serial.printf("[can] --- %s ---\n", dbc::name(*table, message->nameAt));
  for (uint16_t i = 0; i < message->signalCount; i++) {
    const dbc::Signal &signal = table->signals[message->firstSignal + i];
    if (!dbc::isActive(*table, *message, signal, frame.data, frame.length)) continue;
    double value = 0.0;
    if (dbc::read(*table, signal, frame.data, frame.length, value)) {
      Serial.printf("[can] %-38s %12.2f %s\n", dbc::name(*table, signal.nameAt), value,
                    dbc::unit(*table, signal));
    }
  }
}

}  // namespace canui
