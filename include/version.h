// =====================================================================
// version.h - the dash's own version
//
// The firmware had no version string at all, which matters for three
// reasons: a boot log that cannot say what is running, an OTA client with
// nothing to compare against, and a rollback with nothing to report. One
// place to change, printed at boot and in the raw page's title.
// =====================================================================
#pragma once

#define OHX_DASH_VERSION "0.5.1"  // internal RAM: LVGL pool and relay tables in PSRAM, [ram] reports
// 0.5.0 - CAN relay receiver, DBC tables, CAN DATA page, less HTTP

// Date and time the image was built, which is how two builds of the same
// version are told apart on the bench.
#define OHX_DASH_BUILD __DATE__ " " __TIME__
