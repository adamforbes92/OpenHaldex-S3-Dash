// =====================================================================
// can_ui.h - the CAN DATA page
//
// Every id the controller is relaying from the car's bus, with its name, its rate
// and, for the one selected, every signal in it decoded from the DBC. It is how the
// rest of the powertrain CAN gets read without a laptop or a logger.
//
// A separate page, not a dashboard widget: the dashboard's layout does not change.
// See can_ui.cpp.
// =====================================================================
#pragma once

namespace canui {

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

/// Print every id heard, and the selected one's signals, to the console.
void dumpToSerial();

}  // namespace canui
