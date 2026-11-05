// =====================================================================
// raw_ui.h - the raw telemetry page
//
// Everything the dash can see, over both transports, rendered without
// interpretation: a field the C6 marks as "no value" shows "--", never 0.
//
// This is the in-car instrument for the drive test - photograph the panel
// rather than carry a laptop - and the page a tester reads back when
// reporting on a car we cannot reach ourselves.
//
// It is a separate page, not a dashboard widget. See raw_ui.cpp.
// =====================================================================
#pragma once

namespace rawui {

/// Build the page (hidden). Call once, after ui::begin().
void begin();

/// Refresh if the page is up and not frozen. Call every loop().
void tick();

/// Show or hide the page. Showing it takes the dashboard off the glass.
void show(bool visible);

/// Flip between shown and hidden.
void toggle();

/// True while the page is up.
bool visible();

}  // namespace rawui
