// =====================================================================
// board_config.h - Elecrow CrowPanel 7.0" (ESP32-S3) hardware description
//
// Every value here was taken from Elecrow's own 7.0" bring-up example
// (LVGL_Arduino7.0.ino + touch.h) so it matches this exact board.
//
// Board identification (read-only, via esptool + the factory image):
//   ESP32-S3 QFN56 rev v0.2, 8 MB OPI PSRAM, 4 MB quad flash
//   MAC 44:1b:f6:8b:07:98, CH340 USB-UART bridge
//   factory app contains the string "ESP32 Display-7.0"
// =====================================================================
#pragma once

#include <stdint.h>

// ---------------------------------------------------------------- panel
#define BOARD_NAME "CrowPanel 7.0 ESP32-S3"

#define LCD_H_RES 800
#define LCD_V_RES 480

// RGB565 parallel bus. LovyanGFX wants the 16 data lines in this order:
//   [0..4] = B0..B4, [5..10] = G0..G5, [11..15] = R0..R4
// This permutation is specific to the 7.0" board. The 5.0" board uses a
// DIFFERENT permutation of the same pins; see DASH_USE_50_PINMAP below.
#define LCD_PIN_DATA_B0 15
#define LCD_PIN_DATA_B1 7
#define LCD_PIN_DATA_B2 6
#define LCD_PIN_DATA_B3 5
#define LCD_PIN_DATA_B4 4
#define LCD_PIN_DATA_G0 9
#define LCD_PIN_DATA_G1 46
#define LCD_PIN_DATA_G2 3
#define LCD_PIN_DATA_G3 8
#define LCD_PIN_DATA_G4 16
#define LCD_PIN_DATA_G5 1
#define LCD_PIN_DATA_R0 14
#define LCD_PIN_DATA_R1 21
#define LCD_PIN_DATA_R2 47
#define LCD_PIN_DATA_R3 48
#define LCD_PIN_DATA_R4 45

#define LCD_PIN_DE 41    // data enable ("henable")
#define LCD_PIN_VSYNC 40
#define LCD_PIN_HSYNC 39
#define LCD_PIN_PCLK 0

// 16 MHz instead of Elecrow's 24: the panel's DMA reads its frame buffer out
// of PSRAM continuously (800x480x16bpp = 37.9 MB/s at 24 MHz), and this board
// measures only ~35-44 MB/s of total PSRAM bandwidth, so the display was
// eating the whole memory bus and every byte WiFi moved showed up as jitter.
// 16 MHz asks for 25.3 MB/s and still gives 33 refreshes per second, which is
// above film rate. 'p' on the serial console walks 24/20/16/12 MHz at runtime.
#define LCD_PCLK_HZ 16000000  // 800x480 @ ~33 Hz frame rate
#define LCD_HSYNC_POLARITY 0
#define LCD_HSYNC_FRONT_PORCH 40
#define LCD_HSYNC_PULSE_WIDTH 48
#define LCD_HSYNC_BACK_PORCH 40
#define LCD_VSYNC_POLARITY 0
#define LCD_VSYNC_FRONT_PORCH 1
#define LCD_VSYNC_PULSE_WIDTH 31
#define LCD_VSYNC_BACK_PORCH 13
#define LCD_PCLK_ACTIVE_NEG 1

#define LCD_PIN_BACKLIGHT 2  // plain GPIO, active high

// ---------------------------------------------------- I2C control bus
// Shared by the PCA9557 I/O expander and the GT911 touch controller.
#define BOARD_I2C_SDA 19
#define BOARD_I2C_SCL 20
#define BOARD_I2C_HZ 400000

// PCA9557 I/O expander. Only two of its pins matter:
//   IO0 = LCD panel reset (drive low, then high)
//   IO1 = board input line (left as an input)
#define PCA9557_ADDR 0x18  // A2:A0 strapping = 000
#define PCA9557_IO_LCD_RST 0
#define PCA9557_IO_INPUT 1

// --------------------------------------------------------- spare I/O
// GPIO38 is a general purpose output on the CrowPanel, used here as the
// "AUX" line (relay / dimmer / fan). Driven low at boot.
#define BOARD_PIN_AUX 38

// ---------------------------------------------------------- frame buffers
// Two PSRAM frame buffers, with LVGL in DIRECT mode: LVGL still redraws
// only the invalidated rectangles, but it draws them into the buffer the
// panel is NOT scanning, and the swap happens at VSYNC. One buffer is
// enough only if you accept LVGL painting into rows the panel is reading
// out at that moment, which shows up as the image tearing/rolling.
//
// Note the render mode that goes with this: DIRECT (per-rectangle, into a
// full-screen buffer), never FULL. FULL forces a whole 800x480 render plus
// the same 768 KB of PSRAM traffic every frame, which both costs ~7 fps and
// starves the panel's DMA.
#ifndef LCD_USE_DOUBLE_FB
#define LCD_USE_DOUBLE_FB 1
#endif
#define LCD_FB_COUNT (LCD_USE_DOUBLE_FB ? 2 : 1)

// ------------------------------------------------------- RGB DMA buffering
// The RGB peripheral scans the frame buffer out of PSRAM continuously at
// ~41 MB/s (800x480x16bpp at 49 Hz) while the CPU is also reading and
// writing PSRAM for LVGL and WiFi. If the DMA's prefetch falls behind, the
// peripheral loses sync with the DMA and the whole image shifts (Espressif
// calls it "insufficient bandwidth... permanent shift"); with
// CONFIG_LCD_RGB_RESTART_IN_VSYNC it re-syncs every VSYNC, so the picture
// jitters/rolls instead of staying displaced.
//
// The driver hides that behind an internal-SRAM "bounce" buffer that it
// refills from PSRAM in bursts. Deeper = more slack against a WiFi burst.
// The driver allocates TWO of these, so the cost is 2 x lines x 800 x 2 B:
//   10 lines -> 32 KB, 16 lines -> 51 KB, 24 lines -> 77 KB.
// This is the biggest user of internal RAM left, and it has to be internal. Cutting
// it needs a picture to judge by, so change it only with the glass in front of you:
// [ram] after panel on the serial console shows what it took. The driver also needs
// 480 lines to divide exactly by this number (10, 12, 15, 16, 20, 24 ...).
//
// 0 disables the bounce buffer entirely: the RGB DMA then streams the frame
// buffer straight out of PSRAM, which is how LovyanGFX/Elecrow's own example
// drives this panel. That is the "known good" path, but it also removes all
// slack against PSRAM contention, so a starved DMA shifts the image. The
// other cost of enabling it is that draw_bitmap() stops blocking (measured
// 0.06 ms per flush), so LVGL has to be told separately when the frame is
// really done - that is what the frame callback in crowpanel.cpp is for.
// 'b' on the serial console switches between the two at runtime. Measured on
// this board: 0 lines makes the picture roll like a badly tuned TV, so the
// bounce buffer is not optional here.
#ifndef LCD_BOUNCE_LINES
#define LCD_BOUNCE_LINES 24
#endif

// -------------------------------------------------------- pixel clock source
// lcd_clk = src / (div_num + div_b/div_a), and on this peripheral a
// fractional divider is allowed. 24 MHz divides PLL_F160M exactly only as
// 6 + 2/3; PLL_D2 (240 MHz) gives a clean integer 10. An exact divider
// removes any pixel-clock jitter from the picture.
#ifndef LCD_USE_PLL240M
#define LCD_USE_PLL240M 1
#endif

// -------------------------------------------------------------- touch
#define TOUCH_I2C_SDA BOARD_I2C_SDA
#define TOUCH_I2C_SCL BOARD_I2C_SCL
#define TOUCH_GT911_INT -1  // not routed
#define TOUCH_GT911_RST -1  // not routed (panel reset is on the expander)

// The 5.0" board and the 7.0" board wire the same 16 GPIOs to the panel's
// RGB inputs in different orders, and swap DE/VSYNC. If this firmware is
// ever built for a CrowPanel 5.0", set -DDASH_USE_50_PINMAP=1 and the
// alternate bus description below is used instead.
#ifndef DASH_USE_50_PINMAP
#define DASH_USE_50_PINMAP 0
#endif

#if DASH_USE_50_PINMAP
#undef LCD_PIN_DATA_B0
#undef LCD_PIN_DATA_B1
#undef LCD_PIN_DATA_B2
#undef LCD_PIN_DATA_B3
#undef LCD_PIN_DATA_B4
#undef LCD_PIN_DATA_G0
#undef LCD_PIN_DATA_G1
#undef LCD_PIN_DATA_G2
#undef LCD_PIN_DATA_G3
#undef LCD_PIN_DATA_G4
#undef LCD_PIN_DATA_G5
#undef LCD_PIN_DATA_R0
#undef LCD_PIN_DATA_R1
#undef LCD_PIN_DATA_R2
#undef LCD_PIN_DATA_R3
#undef LCD_PIN_DATA_R4
#undef LCD_PIN_DE
#undef LCD_PIN_VSYNC
#undef LCD_PCLK_HZ
#undef LCD_HSYNC_FRONT_PORCH
#undef LCD_HSYNC_PULSE_WIDTH
#undef LCD_HSYNC_BACK_PORCH
#undef LCD_VSYNC_FRONT_PORCH
#undef LCD_VSYNC_PULSE_WIDTH
#undef LCD_VSYNC_BACK_PORCH
#define LCD_PIN_DATA_B0 8
#define LCD_PIN_DATA_B1 3
#define LCD_PIN_DATA_B2 46
#define LCD_PIN_DATA_B3 9
#define LCD_PIN_DATA_B4 1
#define LCD_PIN_DATA_G0 5
#define LCD_PIN_DATA_G1 6
#define LCD_PIN_DATA_G2 7
#define LCD_PIN_DATA_G3 15
#define LCD_PIN_DATA_G4 16
#define LCD_PIN_DATA_G5 4
#define LCD_PIN_DATA_R0 45
#define LCD_PIN_DATA_R1 48
#define LCD_PIN_DATA_R2 47
#define LCD_PIN_DATA_R3 21
#define LCD_PIN_DATA_R4 14
#define LCD_PIN_DE 40
#define LCD_PIN_VSYNC 41
#define LCD_PCLK_HZ 15000000
#define LCD_HSYNC_FRONT_PORCH 8
#define LCD_HSYNC_PULSE_WIDTH 4
#define LCD_HSYNC_BACK_PORCH 43
#define LCD_VSYNC_FRONT_PORCH 8
#define LCD_VSYNC_PULSE_WIDTH 4
#define LCD_VSYNC_BACK_PORCH 12
#endif  // DASH_USE_50_PINMAP
