// =====================================================================
// crowpanel.cpp - Elecrow CrowPanel 7.0" board bring-up
//
// The panel is driven with ESP-IDF's esp_lcd RGB peripheral directly
// rather than through LovyanGFX. Reason: LovyanGFX (1.2.36, the newest
// published release) only supports a single RGB frame buffer, while this
// dashboard needs two so LVGL can render into the back buffer and hand it
// to the panel without tearing. esp_lcd supports num_fbs = 2 natively.
//
// Panel timing and the pin permutation come from Elecrow's 7.0" example,
// and the pin array was independently confirmed against this board's
// factory firmware (byte-identical 16-byte sequence found at 0x1f0240 in
// the 4 MB dump). See README.md section 8.
// =====================================================================
#include "hal/crowpanel.h"

#include <Arduino.h>
#include <TAMC_GT911.h>
#include <Wire.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <lvgl.h>

#include <cstring>

#include "board_config.h"
#include "hal/pca9557.h"

namespace crowpanel {
namespace {

// Panel controller handle, owned for the life of the program.
esp_lcd_panel_handle_t g_panel = nullptr;

// GT911 shares the I2C bus with the expander.
TAMC_GT911 g_touch(TOUCH_I2C_SDA, TOUCH_I2C_SCL, TOUCH_GT911_INT, TOUCH_GT911_RST, LCD_H_RES,
                   LCD_V_RES);
Pca9557 g_expander;

Status g_status = {false, 0, false, false, 0};
bool g_backlightAttached = false;

// ------------------------------------------------- LVGL <-> esp_lcd glue
// LV_DISPLAY_RENDER_MODE_FULL: LVGL renders a whole frame into one of the
// two PSRAM buffers, then this hands that buffer to the RGB peripheral,
// which scans it out from the next VSYNC. No copy, no tearing.
void displayFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *pxMap) {
  (void)area;
  esp_lcd_panel_draw_bitmap(g_panel, 0, 0, LCD_H_RES, LCD_V_RES, pxMap);
  lv_display_flush_ready(disp);
}

void touchRead(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  g_touch.read();
  if (g_touch.isTouched) {
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = (int32_t)g_touch.points[0].x;
    data->point.y = (int32_t)g_touch.points[0].y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ----------------------------------------------------- panel power cycle
// The LCD controller's reset is on the PCA9557, not on a GPIO, so it has to
// be pulsed manually before the RGB peripheral starts driving pixels.
// IO0 low -> 20 ms -> IO0 high -> 100 ms settle, then release IO1 back to
// an input. This is Elecrow's sequence.
void panelReset() {
  g_expander.reset();
  g_expander.pinMode(PCA9557_IO_LCD_RST, false);
  g_expander.pinMode(PCA9557_IO_INPUT, false);
  g_expander.write(PCA9557_IO_LCD_RST, false);
  g_expander.write(PCA9557_IO_INPUT, false);
  delay(20);
  g_expander.write(PCA9557_IO_LCD_RST, true);
  delay(100);
  g_expander.pinMode(PCA9557_IO_INPUT, true);
}

bool startPanel() {
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;

  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 2;  // front + back, both in PSRAM
  cfg.bounce_buffer_size_px = 0;
  cfg.sram_trans_align = 4;
  cfg.psram_trans_align = 64;

  cfg.hsync_gpio_num = LCD_PIN_HSYNC;
  cfg.vsync_gpio_num = LCD_PIN_VSYNC;
  cfg.de_gpio_num = LCD_PIN_DE;
  cfg.pclk_gpio_num = LCD_PIN_PCLK;
  cfg.disp_gpio_num = -1;

  // esp_lcd wants the 16 data lines as B0..B4, G0..G5, R0..R4.
  const int kDataPins[16] = {
      LCD_PIN_DATA_B0, LCD_PIN_DATA_B1, LCD_PIN_DATA_B2, LCD_PIN_DATA_B3, LCD_PIN_DATA_B4,
      LCD_PIN_DATA_G0, LCD_PIN_DATA_G1, LCD_PIN_DATA_G2, LCD_PIN_DATA_G3, LCD_PIN_DATA_G4,
      LCD_PIN_DATA_G5, LCD_PIN_DATA_R0, LCD_PIN_DATA_R1, LCD_PIN_DATA_R2, LCD_PIN_DATA_R3,
      LCD_PIN_DATA_R4,
  };
  memcpy(cfg.data_gpio_nums, kDataPins, sizeof(kDataPins));

  cfg.timings.pclk_hz = LCD_PCLK_HZ;
  cfg.timings.h_res = LCD_H_RES;
  cfg.timings.v_res = LCD_V_RES;
  cfg.timings.hsync_pulse_width = LCD_HSYNC_PULSE_WIDTH;
  cfg.timings.hsync_back_porch = LCD_HSYNC_BACK_PORCH;
  cfg.timings.hsync_front_porch = LCD_HSYNC_FRONT_PORCH;
  cfg.timings.vsync_pulse_width = LCD_VSYNC_PULSE_WIDTH;
  cfg.timings.vsync_back_porch = LCD_VSYNC_BACK_PORCH;
  cfg.timings.vsync_front_porch = LCD_VSYNC_FRONT_PORCH;
  cfg.timings.flags.pclk_active_neg = LCD_PCLK_ACTIVE_NEG;
  cfg.timings.flags.hsync_idle_low = 0;
  cfg.timings.flags.vsync_idle_low = 0;
  cfg.timings.flags.de_idle_high = 0;
  cfg.timings.flags.pclk_idle_high = 0;

  cfg.flags.fb_in_psram = 1;

  const esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &g_panel);
  if (err != ESP_OK || g_panel == nullptr) {
    Serial.printf("[board] esp_lcd_new_rgb_panel failed: %d\n", (int)err);
    return false;
  }
  esp_lcd_panel_reset(g_panel);
  esp_lcd_panel_init(g_panel);
  esp_lcd_panel_disp_on_off(g_panel, true);
  return true;
}

}  // namespace

bool begin() {
  // I2C first: the expander holds the panel in reset.
  Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL, BOARD_I2C_HZ);

  g_status.expanderOk = g_expander.begin(PCA9557_ADDR);
  g_status.expanderAddr = g_expander.address();
  if (!g_status.expanderOk) {
    Serial.printf("[board] PCA9557 NOT FOUND (SDA=%d SCL=%d) - panel stays in reset\n",
                  BOARD_I2C_SDA, BOARD_I2C_SCL);
    return false;
  }
  Serial.printf("[board] PCA9557 at 0x%02X\n", g_status.expanderAddr);
  panelReset();

  // GPIO38 is a plain output on this board; keep it low until asked.
  pinMode(BOARD_PIN_AUX, OUTPUT);
  digitalWrite(BOARD_PIN_AUX, LOW);

  const uint32_t psramBefore = ESP.getFreePsram();
  g_status.panelOk = startPanel();
  if (!g_status.panelOk) return false;

  // Hand the two driver-allocated frame buffers to LVGL.
  void *fb0 = nullptr;
  void *fb1 = nullptr;
  const esp_err_t fbErr = esp_lcd_rgb_panel_get_frame_buffer(g_panel, 2, &fb0, &fb1);
  if (fbErr != ESP_OK || !fb0 || !fb1) {
    Serial.printf("[board] only %s frame buffer available (err %d)\n", fb0 ? "one" : "no",
                  (int)fbErr);
    return false;
  }
  g_status.minFbFree = (int32_t)ESP.getFreePsram();
  Serial.printf("[board] RGB panel up: %dx%d @ %u Hz, 2 fbs %p/%p, PSRAM used %u B\n", LCD_H_RES,
                LCD_V_RES, (unsigned)LCD_PCLK_HZ, fb0, fb1,
                (unsigned)(psramBefore - ESP.getFreePsram()));

  // Backlight on before the first frame so the panel never shows garbage.
  setBacklight(100);

  lv_init();
  lv_tick_set_cb(millis);

  lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
  if (!disp) {
    Serial.println("[board] lv_display_create failed");
    return false;
  }
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(disp, displayFlush);
  lv_display_set_buffers(disp, fb0, fb1, LCD_H_RES * LCD_V_RES * sizeof(lv_color_t),
                         LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_default(disp);

  // Touch shares the I2C bus, so it comes up after the reset sequence.
  g_touch.begin();
  g_touch.setRotation(ROTATION_NORMAL);
  g_status.touchOk = true;  // the GT911 has no reliable presence query here

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchRead);
  lv_indev_set_display(indev, disp);

  Serial.printf("[board] LVGL %d.%d.%d, free PSRAM %u B\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
                LVGL_VERSION_PATCH, (unsigned)ESP.getFreePsram());
  return true;
}

void setBacklight(uint8_t percent) {
  if (percent > 100) percent = 100;
  if (!g_backlightAttached) {
    ledcAttach(LCD_PIN_BACKLIGHT, 5000, 8);
    g_backlightAttached = true;
  }
  const uint32_t duty = (percent == 0) ? 0 : (uint32_t)((percent * 255U) / 100U);
  ledcWrite(LCD_PIN_BACKLIGHT, duty);
}

void auxWrite(bool on) { digitalWrite(BOARD_PIN_AUX, on ? HIGH : LOW); }

uint32_t psramFree() { return ESP.getFreePsram(); }

const Status &status() { return g_status; }

}  // namespace crowpanel
