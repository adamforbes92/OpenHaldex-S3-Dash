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

#define LCD_PCLK_HZ 24000000  // 800x480 @ ~33 Hz frame rate
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
