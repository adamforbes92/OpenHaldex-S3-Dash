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

}  // namespace ui
