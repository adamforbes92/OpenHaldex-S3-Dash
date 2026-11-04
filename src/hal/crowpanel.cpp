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
uint32_t g_pclkHz = LCD_PCLK_HZ;
lcd_clock_source_t g_clkSrc = LCD_CLK_SRC_DEFAULT;

// ------------------------------------------------- LVGL <-> esp_lcd glue
// LVGL renders into one of the two full-screen PSRAM buffers and calls this
// to hand that buffer to the RGB peripheral. draw_bitmap() does not return
// until the peripheral has picked the buffer up at a frame boundary, so
// flush_ready() below is only reached once the swap is safe; that is what
// keeps the picture free of tearing.
volatile uint32_t g_flushUs = 0;

// The buffer that was handed over last, i.e. the one the panel is scanning
// right now. Kept only so the debug dump below shows what is on the glass.
const void *g_displayedFb = nullptr;

void displayFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *pxMap) {
  (void)area;
  const uint32_t t0 = micros();
  g_displayedFb = pxMap;
#if LCD_USE_DOUBLE_FB
  // Whole-frame buffer: hand it to the panel, which starts scanning it at
  // the next VSYNC.
  esp_lcd_panel_draw_bitmap(g_panel, 0, 0, LCD_H_RES, LCD_V_RES, pxMap);
#else
  // Single frame buffer in DIRECT mode: LVGL has already drawn the dirty
  // rectangles straight into the buffer the panel is scanning, so there is
  // nothing to hand over. Calling draw_bitmap here would memcpy the entire
  // 768 KB screen for no reason.
  (void)pxMap;
#endif
  g_flushUs += micros() - t0;
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

bool configurePanel(const lcd_clock_source_t clkSrc) {
  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = clkSrc;

  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
#if LCD_USE_DOUBLE_FB
  cfg.num_fbs = 2;  // front + back, both in PSRAM
#else
  cfg.num_fbs = 1;  // one PSRAM frame buffer, drawn in place
#endif
  // The panel's DMA otherwise streams straight out of PSRAM continuously,
  // which starves the CPU of the memory bandwidth LVGL needs. The driver's
  // internal-SRAM bounce buffer makes those reads bursty; its depth is the
  // margin against a WiFi burst emptying it mid-frame.
  cfg.bounce_buffer_size_px = LCD_H_RES * LCD_BOUNCE_LINES;
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

  g_panel = nullptr;
  const esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &g_panel);
  if (err != ESP_OK || g_panel == nullptr) {
    Serial.printf("[board] esp_lcd_new_rgb_panel failed: %d\n", (int)err);
    return false;
  }
  esp_lcd_panel_reset(g_panel);
  esp_lcd_panel_init(g_panel);
  esp_lcd_panel_disp_on_off(g_panel, true);
  g_pclkHz = LCD_PCLK_HZ;
  g_clkSrc = clkSrc;
  return true;
}

// Espressif's RGB driver happily builds more than any panel can display;
// 24 MHz is right at the outer edge, so this matters when comparing.
uint32_t refreshHz(const uint32_t pclkHz) {
  const uint32_t htotal =
      LCD_H_RES + LCD_HSYNC_PULSE_WIDTH + LCD_HSYNC_BACK_PORCH + LCD_HSYNC_FRONT_PORCH;
  const uint32_t vtotal =
      LCD_V_RES + LCD_VSYNC_PULSE_WIDTH + LCD_VSYNC_BACK_PORCH + LCD_VSYNC_FRONT_PORCH;
  return (uint32_t)((uint64_t)pclkHz / ((uint64_t)htotal * vtotal));
}

const char *clkSrcName(const lcd_clock_source_t s) {
  switch (s) {
    case LCD_CLK_SRC_PLL240M: return "PLL_D2/240M";
    case LCD_CLK_SRC_PLL160M: return "PLL_F160M";
    case LCD_CLK_SRC_XTAL: return "XTAL";
    default: return "?";
  }
}

// PLL_D2 (240 MHz) divides 24 MHz by an exact 10. PLL_F160M can only reach
// it with the fractional divider 6 + 2/3. Exact is preferable, but fall
// back rather than leave the user with a dead screen.
bool startPanel() {
#if LCD_USE_PLL240M
  if (configurePanel(LCD_CLK_SRC_PLL240M)) return true;
  Serial.println("[board] PLL_D2 clock source unavailable, falling back to PLL_F160M");
#endif
  return configurePanel(LCD_CLK_SRC_DEFAULT);
}

}  // namespace

bool begin() {
  // I2C first: the expander holds the panel in reset.
  // The GT911 reads its whole 185-byte configuration block in one
  // transaction and the Arduino default buffer is only 128 bytes, which
  // makes the touch controller fail to initialise.
  Wire.setBufferSize(256);
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

  // Take the driver's frame buffer(s) and give them to LVGL.
  void *fb0 = nullptr;
  void *fb1 = nullptr;
  const esp_err_t fbErr = esp_lcd_rgb_panel_get_frame_buffer(g_panel, LCD_FB_COUNT, &fb0, &fb1);
  if (fbErr != ESP_OK || !fb0) {
    Serial.printf("[board] no frame buffer from esp_lcd (err %d)\n", (int)fbErr);
    return false;
  }
#if LCD_USE_DOUBLE_FB
  if (!fb1) {
    Serial.println("[board] second frame buffer missing");
    return false;
  }
#endif
  g_status.minFbFree = (int32_t)ESP.getFreePsram();
  Serial.printf("[board] RGB panel up: %dx%d @ %u Hz pclk (%u Hz refresh, %s, %s), %d fb\n", LCD_H_RES,
                LCD_V_RES, (unsigned)g_pclkHz, (unsigned)refreshHz(g_pclkHz),
                clkSrcName(g_clkSrc), LCD_USE_PLL240M ? "exact" : "frac", LCD_FB_COUNT);
  Serial.printf("[board] bounce buffer %d lines (%u B x2), PSRAM used %u B, heap %u B\n",
                LCD_BOUNCE_LINES, (unsigned)(LCD_H_RES * LCD_BOUNCE_LINES * 2),
                (unsigned)(psramBefore - ESP.getFreePsram()), (unsigned)ESP.getFreeHeap());

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
#if LCD_USE_DOUBLE_FB
  // Two full-screen buffers in DIRECT mode: LVGL redraws only the
  // invalidated rectangles, but into whichever buffer the panel is not
  // scanning. displayFlush() then hands that buffer over and the swap
  // lands on a VSYNC, so a frame is never half old / half new.
  lv_display_set_buffers(disp, fb0, fb1, LCD_H_RES * LCD_V_RES * sizeof(lv_color_t),
                         LV_DISPLAY_RENDER_MODE_DIRECT);
#else
  // DIRECT mode: LVGL redraws only the invalidated rectangles, straight
  // into the buffer the panel is scanning. Nothing is copied anywhere.
  lv_display_set_buffers(disp, fb0, nullptr, LCD_H_RES * LCD_V_RES * sizeof(lv_color_t),
                         LV_DISPLAY_RENDER_MODE_DIRECT);
#endif
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

uint32_t takeFlushMicros() {
  const uint32_t v = g_flushUs;
  g_flushUs = 0;
  return v;
}

// ASCII "screenshot": samples the frame buffer the panel is scanning and
// prints it as luminance levels. This is the only way to inspect what the
// dashboard actually renders when the board is on a bench without a camera.
void dumpFrame(int cols, int rows) {
  if (g_displayedFb == nullptr) {
    Serial.println("[dump] nothing flushed yet");
    return;
  }
  if (cols < 20 || cols > 200) cols = 20;
  if (rows < 8 || rows > 60) rows = 8;
  static const char kRamp[] = " .:-=+*#%@";
  const uint16_t *fb = (const uint16_t *)g_displayedFb;
  for (int r = 0; r < rows; r++) {
    const int y0 = (LCD_V_RES * r) / rows;
    const int y1 = (LCD_V_RES * (r + 1)) / rows;
    char line[201];
    for (int c = 0; c < cols; c++) {
      const int x0 = (LCD_H_RES * c) / cols;
      const int x1 = (LCD_H_RES * (c + 1)) / cols;
      uint32_t sum = 0;
      uint32_t n = 0;
      for (int y = y0; y < y1; y++) {
        const uint16_t *row = fb + (size_t)y * LCD_H_RES;
        for (int x = x0; x < x1; x++) {
          const uint16_t p = row[x];
          const uint32_t red = (p >> 11) & 0x1F;
          const uint32_t green = (p >> 5) & 0x3F;
          const uint32_t blue = p & 0x1F;
          sum += (red * 77U + green * 150U + blue * 29U) >> 8;  // 0..~63
          n++;
        }
      }
      const uint32_t lum = n ? (sum / n) : 0;
      int idx = (int)((lum * 9U + 31U) / 63U);
      if (idx > 9) idx = 9;
      line[c] = kRamp[idx];
    }
    line[cols] = '\0';
    Serial.println(line);
  }
}

uint32_t pclkHz() { return g_pclkHz; }

/// Pixel clock of the RGB peripheral and its estimated refresh rate.
const char *clkSrcName() { return clkSrcName(g_clkSrc); }

uint32_t refreshHzNow() { return refreshHz(g_pclkHz); }

// The RGB peripheral's PCLK can be changed on the fly; the driver applies
// it at the next VSYNC. Lowering it cuts the panel's PSRAM appetite
// proportionally, which is the direct lever on DMA starvation.
bool setPclk(uint32_t hz) {
  if (g_panel == nullptr || hz < 5000000 || hz > 30000000) return false;
  if (esp_lcd_rgb_panel_set_pclk(g_panel, hz) != ESP_OK) return false;
  g_pclkHz = hz;
  return true;
}

}  // namespace crowpanel
