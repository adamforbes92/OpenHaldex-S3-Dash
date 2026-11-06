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

#include "theme.h"  // layout::Variant, chosen from what the car can feed

namespace ui {

/// Build the whole screen. Call once, after crowpanel::begin().
void begin();

/// Advance animation / refresh the readouts. Call every loop().
void tick();

/// Show or hide the settings page.
void showSettings(bool visible);

/// True while the settings page is open.
bool settingsOpen();

/// Which page owns the glass. The dashboard is hidden - not merely covered -
/// while another page is up: LVGL 9 has no "covered by an opaque object"
/// skip, so anything left visible underneath is redrawn along with whatever
/// is on top of it. A page that takes the glass must call setPage(), and
/// hand the glass back with setPage(Page::Dashboard).
enum class Page : uint8_t {
  Dashboard,
  Settings,
  Raw,  // the raw telemetry page (src/ui/raw_ui.cpp)
};

void setPage(Page p);
Page page();

/// Place the tile band: four panels, or three when the car has no boost
/// source (theme.h). Called once when the capability decision settles, and
/// by hand from the console. Idempotent.
void applyLayout(layout::Variant v);
layout::Variant layoutVariant();

/// Step the Haldex mode on: the same thing a tap on the HALDEX panel does.
/// Stock -> FWD -> 50:50 -> 60:40 -> 75:25 -> Expert -> Stock, skipping Stock on
/// a standalone controller. The request
/// goes to the controller over ESP-NOW and its answer appears on the panel's
/// sub-line. Exposed so the console can drive it for bench work.
void cycleHaldexMode();

/// Bring-up aid: bit mask selecting which redraw groups stay enabled.
/// 1 = rev bar, 2 = numbers under the rev bar, 4 = the big rev number,
/// 8 = tile readouts, 16 = status bar. Clearing a bit stops that group from
/// invalidating anything, which is how the render cost of each one is
/// measured on real hardware. 0xFFFF (the default) draws everything.
/// Number of times the live data has gone stale, and the oldest live data
/// seen while it was stale (ms). Bench instrument for the link dropping.
uint32_t staleEvents();
uint32_t maxStaleAgeMs();

void setDebugMask(uint16_t mask);
uint16_t debugMask();

/// Microseconds spent in each phase of tick() since the last call.
struct Timings {
  uint32_t gaugeUs, readoutsUs, connUs;
  uint32_t calls;
};
Timings takeTimings();

}  // namespace ui
