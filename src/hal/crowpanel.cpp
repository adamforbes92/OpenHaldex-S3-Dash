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
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
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
uint32_t g_bounceLines = LCD_BOUNCE_LINES;
lcd_clock_source_t g_clkSrc = LCD_CLK_SRC_DEFAULT;
// Kept so the frame buffers can be re-registered when the panel is rebuilt
// with a different bounce depth ('b' on the serial console).
lv_display_t *g_disp = nullptr;

// ------------------------------------------------- LVGL <-> esp_lcd glue
// LVGL renders into one of the two full-screen PSRAM buffers and calls this
// to hand that buffer to the RGB peripheral. draw_bitmap() does not return
// until the peripheral has picked the buffer up at a frame boundary, so
// flush_ready() below is only reached once the swap is safe; that is what
// keeps the picture free of tearing.
volatile uint32_t g_flushUs = 0;
volatile uint32_t g_waitUs = 0;

// The flush callback has to wait for the panel to actually take the buffer it
// just handed over. Two things go wrong without that wait:
//   1. LVGL calls the flush callback once per invalidated AREA and only swaps
//      its buffers on the last one, so pushing on every call hands the panel
//      half-updated frames and re-arms its DMA several times per frame.
//   2. flush_ready() tells LVGL the buffer is free, so it immediately starts
//      drawing into the other one - which the panel may still be scanning.
// esp_lvgl_port solves both the same way: push only on the last call, block
// until the panel reports the frame complete, then flush_ready.
SemaphoreHandle_t g_frameSem = nullptr;
volatile uint32_t g_vsyncCount = 0;

bool IRAM_ATTR onPanelFrameDone(esp_lcd_panel_handle_t panel,
                                const esp_lcd_rgb_panel_event_data_t *edata, void *ctx) {
  (void)panel;
  (void)edata;
  (void)ctx;
  g_vsyncCount++;
  BaseType_t higherWoken = pdFALSE;
  if (g_frameSem != nullptr) {
    xSemaphoreGiveFromISR(g_frameSem, &higherWoken);
  }
  return higherWoken == pdTRUE;
}

// Which callback reports "this frame buffer is free" depends on the DMA path:
// with a bounce buffer it is the bounce-frame one, without it the VSYNC.
bool registerPanelCallbacks() {
  if (g_frameSem == nullptr) {
    g_frameSem = xSemaphoreCreateBinary();
    if (g_frameSem == nullptr) {
      Serial.println("[board] no semaphore for the frame handshake");
      return false;
    }
  }
  // on_vsync fires exactly once per frame, which is what makes the wait below
  // equal to one panel frame (49 Hz -> 20.4 ms). on_frame_buf_complete was
  // tried first and reported ~6 ms, i.e. it is not the per-frame boundary.
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = onPanelFrameDone;
  const esp_err_t err = esp_lcd_rgb_panel_register_event_callbacks(g_panel, &cbs, nullptr);
  if (err != ESP_OK) {
    Serial.printf("[board] frame callbacks failed: %d\n", (int)err);
    return false;
  }
  return true;
}

// The buffer that was handed over last, i.e. the one the panel is scanning
// right now. Kept only so the debug dump below shows what is on the glass.
const void *g_displayedFb = nullptr;

void displayFlush(lv_display_t *disp, const lv_area_t *area, uint8_t *pxMap) {
  (void)area;
  const uint32_t t0 = micros();
  if (lv_display_flush_is_last(disp)) {
    g_displayedFb = pxMap;
    esp_lcd_panel_draw_bitmap(g_panel, 0, 0, LCD_H_RES, LCD_V_RES, pxMap);
    const uint32_t t1 = micros();
    if (g_frameSem != nullptr) {
      // Drop a notification left over from an earlier frame, then wait for
      // this one to be picked up. The timeout is only a deadlock guard.
      xSemaphoreTake(g_frameSem, 0);
      xSemaphoreTake(g_frameSem, pdMS_TO_TICKS(60));
    }
    g_waitUs += micros() - t1;
  }
  g_flushUs += micros() - t0;
  lv_display_flush_ready(disp);
}

// Running record of the touch samples (see crowpanel.h). Declared before
// touchRead() because that is what fills it in.
TouchProbe g_probe = {0, 0, 0, INT32_MAX, INT32_MIN, INT32_MAX, INT32_MIN, 0, 0, false};
uint8_t g_touchRot = 0;

void touchRead(lv_indev_t *indev, lv_indev_data_t *data) {
  (void)indev;
  const uint32_t t0 = micros();
  g_touch.read();
  const uint32_t dtUs = micros() - t0;
  if (dtUs > g_probe.maxReadUs) g_probe.maxReadUs = dtUs;

  const bool pressed = g_touch.isTouched;
  if (pressed) {
    const int32_t x = (int32_t)g_touch.points[0].x;
    const int32_t y = (int32_t)g_touch.points[0].y;
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
    g_probe.count++;
    if (x < g_probe.minX) g_probe.minX = x;
    if (x > g_probe.maxX) g_probe.maxX = x;
    if (y < g_probe.minY) g_probe.minY = y;
    if (y > g_probe.maxY) g_probe.maxY = y;
    g_probe.lastX = x;
    g_probe.lastY = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
  if (pressed != g_probe.lastPressed) {
    g_probe.lastPressed = pressed;
    g_probe.edges++;
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
  cfg.bounce_buffer_size_px = g_bounceLines ? LCD_H_RES * g_bounceLines : 0;
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
  registerPanelCallbacks();
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

// ------------------------------------------------------- touch bench aids
// The controller's axes need not line up with the panel's, and the library
// handles direction in software: readPoint() returns the raw point mirrored
// for ROTATION_NORMAL, mirrored on one axis for LEFT/RIGHT and untouched for
// ROTATION_INVERTED. Which one is right for this glass has to be measured,
// not guessed, and 'r' on the console walks the four live.
TouchProbe touchProbe() { return g_probe; }

void resetTouchProbe() {
  g_probe.count = 0;
  g_probe.edges = 0;
  g_probe.maxReadUs = 0;
  g_probe.minX = INT32_MAX;
  g_probe.maxX = INT32_MIN;
  g_probe.minY = INT32_MAX;
  g_probe.maxY = INT32_MIN;
  g_probe.lastPressed = false;
}

const char *cycleTouchRotation() {
  static const uint8_t rots[] = {ROTATION_NORMAL, ROTATION_INVERTED, ROTATION_LEFT, ROTATION_RIGHT};
  static const char *names[] = {"mirror both (w-x, h-y)", "none (x, y)", "left (w-y, x)",
                                "right (y, h-x)"};
  g_touchRot = (uint8_t)((g_touchRot + 1) % 4);
  g_touch.setRotation(rots[g_touchRot]);
  resetTouchProbe();
  return names[g_touchRot];
}

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
  Serial.printf("[board] bounce buffer %u lines, PSRAM used %u B, heap %u B\n",
                (unsigned)g_bounceLines, (unsigned)(psramBefore - ESP.getFreePsram()),
                (unsigned)ESP.getFreeHeap());

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
  g_disp = disp;

  // Touch shares the I2C bus, so it comes up after the reset sequence.
  g_touch.begin();
  // TAMC_GT911::begin() calls Wire.begin(sda, scl) with no clock. On this
  // core that only warns ("Bus already started") rather than re-initialising,
  // so the 400 kHz set above survives - but say it again rather than depend on
  // that, because every touch sample is three round trips.
  Wire.setClock(BOARD_I2C_HZ);

  // ROTATION_INVERTED is the one that leaves the controller's axes alone, and
  // that is what this glass needs. The other three move the point: measured on
  // this panel, a finger held in the bottom-right corner (785, 475) arrived as
  // (15, 5) under the library's own default (ROTATION_NORMAL, which returns
  // width-x, height-y). Every press was therefore landing 180 degrees away
  // from the finger: taps hit whatever was opposite them - so no button ever
  // fired - while the long press still worked, because it is on the whole top
  // band and a mirrored press usually lands inside it.
  g_touch.setRotation(ROTATION_INVERTED);
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

uint32_t takeWaitMicros() {
  const uint32_t v = g_waitUs;
  g_waitUs = 0;
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

/// Rebuild the RGB panel with a different bounce-buffer depth, reusing the
/// same LVGL display. Used to A/B the two DMA paths on a real screen: the
/// bounce buffer decouples the DMA from PSRAM latency but makes the flush
/// asynchronous, and the direct path does the opposite.
bool setBounceLines(uint32_t lines) {
  if (g_disp == nullptr) return false;
  if (lines > 60) lines = 60;
  if (g_panel != nullptr) {
    esp_lcd_panel_del(g_panel);
    g_panel = nullptr;
  }
  const uint32_t keepClk = g_pclkHz;
  const lcd_clock_source_t keepSrc = g_clkSrc;
  g_bounceLines = lines;
  if (!configurePanel(keepSrc)) {
    Serial.println("[board] panel rebuild FAILED - display is down until reset");
    return false;
  }
  (void)keepClk;
  void *fb0 = nullptr;
  void *fb1 = nullptr;
  if (esp_lcd_rgb_panel_get_frame_buffer(g_panel, LCD_FB_COUNT, &fb0, &fb1) != ESP_OK || !fb0) {
    Serial.println("[board] rebuild: no frame buffer");
    return false;
  }
  lv_display_set_buffers(g_disp, fb0, (LCD_FB_COUNT > 1) ? fb1 : nullptr,
                         LCD_H_RES * LCD_V_RES * sizeof(lv_color_t),
                         LV_DISPLAY_RENDER_MODE_DIRECT);
  // Both buffers now hold stale pixels; repaint everything.
  lv_obj_invalidate(lv_screen_active());
  Serial.printf("[board] panel rebuilt: bounce %u lines, pclk %u Hz, heap %u B\n", (unsigned)lines,
                (unsigned)g_pclkHz, (unsigned)ESP.getFreeHeap());
  return true;
}

uint32_t bounceLines() { return g_bounceLines; }

uint32_t takeVsyncCount() {
  const uint32_t v = g_vsyncCount;
  g_vsyncCount = 0;
  return v;
}

// How much PSRAM bandwidth is actually available. The RGB panel reads
// 800x480x2 B at 49 Hz = 37.9 MB/s continuously, so if these numbers are not
// comfortably above that, the panel's DMA is competing with the CPU for the
// same memory - which is the difference between a stable picture and one that
// rolls. Buffers are large enough (256 KB) to swamp the cache.
void psramBandwidthProbe() {
  const size_t bytes = 256 * 1024;
  uint8_t *a = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  uint8_t *b = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (a == nullptr || b == nullptr) {
    Serial.println("[board] PSRAM bandwidth probe skipped (no memory)");
    if (a) heap_caps_free(a);
    if (b) heap_caps_free(b);
    return;
  }
  memset(a, 0x5A, bytes);
  const uint32_t t0 = micros();
  memset(b, 0xA5, bytes);
  const uint32_t t1 = micros();
  memcpy(a, b, bytes);
  const uint32_t t2 = micros();
  const float writeMBs = (float)bytes / (float)(t1 - t0);
  const float copyMBs = (float)bytes / (float)(t2 - t1);
  Serial.printf("[board] PSRAM: %.0f MB/s write, %.0f MB/s copy (panel needs ~38 MB/s)\n",
                writeMBs, copyMBs);
  heap_caps_free(a);
  heap_caps_free(b);
}

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
