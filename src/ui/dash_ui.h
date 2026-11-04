// =====================================================================
// dash_ui.h - the LVGL dashboard
//
// A native port of dash.html. The geometry comes from the same formulas
// (see theme.h) so the panel layout matches the approved design.
//
// Calling convention: everything here runs on the Arduino loop task
// (core 1). No LVGL call may happen from the network task.
// =====================================================================
#pragma once

#include <lvgl.h>

namespace ui {

/// Build the whole screen. Call once, after crowpanel::begin().
void begin();

/// Advance animation / refresh the readouts. Call every loop().
void tick();

/// Show or hide the settings page.
void showSettings(bool visible);

/// True while the settings page is open.
bool settingsOpen();

/// Bring-up aid: bit mask selecting which redraw groups stay enabled.
/// 1 = rev bar, 2 = boost bar, 4 = numbers under the rev bar, 8 = the two
/// big numbers, 16 = tile readouts, 32 = status bar. Clearing a bit stops
/// that group from invalidating anything, which is how the render cost of
/// each one is measured on real hardware. 0xFFFF (the default) draws
/// everything.
/// Number of times the live data has gone stale, and the oldest live data
/// seen while it was stale (ms). Bench instrument for the link dropping.
uint32_t staleEvents();
uint32_t maxStaleAgeMs();

void setDebugMask(uint16_t mask);
uint16_t debugMask();

/// Microseconds spent in each phase of tick() since the last call.
struct Timings {
  uint32_t barsUs, readoutsUs, connUs;
  uint32_t calls;
};
Timings takeTimings();

}  // namespace ui
