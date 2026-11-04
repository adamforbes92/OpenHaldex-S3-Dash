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
/// 1 = zone arcs, 2 = needle, 4 = ticks+numbers, 8 = rpm digits + shift
/// lamp, 16 = tile readouts, 32 = status bar. Clearing a bit stops that
/// group from invalidating anything, which is how the render cost of each
/// one is measured on real hardware. 0xFFFF (the default) draws everything.
void setDebugMask(uint16_t mask);
uint16_t debugMask();

/// Microseconds spent in each phase of tick() since the last call.
struct Timings {
  uint32_t tachUs, readoutsUs, connUs;
  uint32_t calls;
};
Timings takeTimings();

}  // namespace ui
