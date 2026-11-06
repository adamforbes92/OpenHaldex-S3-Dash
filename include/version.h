// =====================================================================
// version.h - the dash's own version
//
// The firmware had no version string at all, which matters for three
// reasons: a boot log that cannot say what is running, an OTA client with
// nothing to compare against, and a rollback with nothing to report. One
// place to change, printed at boot and in the raw page's title.
// =====================================================================
#pragma once

#define OHX_DASH_VERSION "0.4.2"  // settings survive a reboot, one controller, radio stays up on a rejoin

// Date and time the image was built, which is how two builds of the same
// version are told apart on the bench.
#define OHX_DASH_BUILD __DATE__ " " __TIME__
