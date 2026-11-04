// =====================================================================
// crowpanel.h - board bring-up for the Elecrow CrowPanel 7.0" (ESP32-S3)
//
// Owns everything below the UI:
//   * I2C bus + PCA9557 expander (LCD panel reset)
//   * esp_lcd RGB panel driver with two PSRAM frame buffers
//   * LVGL display + pointer input registered straight onto those buffers
//   * backlight and the spare GPIO38 output
// =====================================================================
#pragma once

#include <stdint.h>

namespace crowpanel {

/// Power-on sequence: I2C, expander, panel reset, RGB bus, backlight,
/// LVGL display + touch. Safe to call once from setup().
/// Returns false if the panel could not be started (the caller should keep
/// going anyway so the serial console stays usable).
bool begin();

/// Backlight brightness, 0..100 %. 0 fully off, 100 fully on.
void setBacklight(uint8_t percent);

/// The spare GPIO38 output on the CrowPanel (relay / dimmer / fan).
void auxWrite(bool on);

/// Free PSRAM in bytes, for the boot banner.
uint32_t psramFree();

/// Microseconds spent inside the LVGL flush callback since the last call,
/// then resets the counter. Used to tell "LVGL is slow" apart from
/// "handing the frame to the panel is slow".
uint32_t takeFlushMicros();

/// Microseconds of the above that were spent waiting for the panel to take
/// the frame (the VSYNC handshake), then resets it.
uint32_t takeWaitMicros();

/// VSYNC events since the last call, then resets the counter. The panel
/// refresh rate is this divided by the interval.
uint32_t takeVsyncCount();

/// Print measured PSRAM write/copy bandwidth (bench aid for DMA starvation).
void psramBandwidthProbe();

/// Live PCLK of the RGB peripheral, its source, and the refresh rate that
/// follows from the porch values, for logging.
uint32_t pclkHz();
uint32_t refreshHzNow();
const char *clkSrcName();

/// Change the pixel clock at runtime (applied by the driver at the next
/// VSYNC). The panel's PSRAM read bandwidth scales with it, so this is the
/// handle for chasing DMA starvation without a reflash. 5..30 MHz.
bool setPclk(uint32_t hz);

/// Rebuild the panel with a different bounce-buffer depth (0 = the DMA
/// streams PSRAM directly). Returns false if the rebuild failed.
bool setBounceLines(uint32_t lines);
uint32_t bounceLines();

/// Print the frame buffer the panel is currently scanning as an ASCII
/// luminance map (cols x rows cells). Bench aid: it shows what the screen
/// really contains, without a camera.
void dumpFrame(int cols, int rows);

/// Static description of what came up, for logging.
struct Status {
  bool expanderOk;
  uint8_t expanderAddr;
  bool panelOk;
  bool touchOk;
  int32_t minFbFree;  // bytes of PSRAM left after the frame buffers
};
const Status &status();

}  // namespace crowpanel
