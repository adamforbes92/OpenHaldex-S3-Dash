// =====================================================================
// main.cpp - OpenHaldex S3 Dash
//
// A native dashboard for the Elecrow CrowPanel 7.0" (ESP32-S3), driven by
// the unmodified OpenHaldex-C6 HTTP API.
//
// Task layout:
//   core 1  Arduino loop  -> LVGL rendering + dashboard refresh
//   core 0  "ohxnet"      -> WiFi + /api/live (10 Hz) + /api/dashboard (1 Hz)
//
// Keeping the network on the second core is the main reason this runs on
// the S3 at all: the C6 is single-core, so there a 10 Hz JSON poll competes
// directly with CAN parsing.
// =====================================================================
#include <Arduino.h>
#include <esp_heap_caps.h>

#include "board_config.h"
#include "hal/crowpanel.h"
#include "net/telemetry.h"
#include "app/config.h"
#include "ui/dash_ui.h"

namespace {

// UI refresh ceiling. The data arrives at 10 Hz; rendering faster than
// ~30 fps only burns CPU, because the RGB panel scans out at ~33 fps.
constexpr uint32_t kUiPeriodMs = 33;

uint32_t g_lastUiMs = 0;
uint32_t g_lastStatsMs = 0;
uint32_t g_frames = 0;
uint32_t g_renderUs = 0;

void printBanner() {
  Serial.println();
  Serial.println("=========================================");
  Serial.println(" OpenHaldex S3 Dash");
  Serial.printf(" board    : %s\n", BOARD_NAME);
  Serial.printf(" chip     : %s rev %d, %d core(s) @ %u MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), getCpuFrequencyMhz());
  Serial.printf(" flash    : %u MB\n", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  Serial.printf(" psram    : %u MB free of %u MB\n",
                (unsigned)(ESP.getFreePsram() / (1024 * 1024)),
                (unsigned)(ESP.getPsramSize() / (1024 * 1024)));
  Serial.printf(" heap     : %u B free\n", (unsigned)ESP.getFreeHeap());
  Serial.printf(" sdk      : %s\n", ESP.getSdkVersion());
  Serial.println("=========================================");
}

void printStats() {
  const telemetry::Stats s = telemetry::stats();
  const cfg::Data &c = cfg::get();

  Serial.printf("[stats] live %lu ok / %lu fail | dash %lu ok / %lu fail | reconnects %lu\n",
                (unsigned long)s.liveOk, (unsigned long)s.liveFail, (unsigned long)s.dashOk,
                (unsigned long)s.dashFail, (unsigned long)s.reconnects);
  Serial.printf("[stats] wifi %s rssi %d  ip %u.%u.%u.%u  ->  %s:%u  (%u/%u ms)\n",
                s.wifiUp ? "up" : "DOWN", (int)s.rssi, (s.ip >> 0) & 0xFF, (s.ip >> 8) & 0xFF,
                (s.ip >> 16) & 0xFF, (s.ip >> 24) & 0xFF, c.host, c.port, c.pollLiveMs,
                c.pollDashMs);
  const ui::Timings t = ui::takeTimings();
  const uint32_t flushUs = crowpanel::takeFlushMicros();
  const uint32_t n = t.calls ? t.calls : 1;
  Serial.printf("[stats] ms/tick: tach %.2f readouts %.2f conn %.2f  (mask 0x%04X)\n",
                (t.tachUs / 1000.0f) / n, (t.readoutsUs / 1000.0f) / n,
                (t.connUs / 1000.0f) / n, ui::debugMask());
  Serial.printf("[stats] render %.1f fps | %.1f ms/loop | %.2f ms/flush | heap %u B, psram %u B\n",
                g_frames * 1000.0f / 5000.0f,
                (g_renderUs / 1000.0f) / (g_frames ? g_frames : 1),
                (flushUs / 1000.0f) / (g_frames ? g_frames : 1), (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getFreePsram());
  g_frames = 0;
  g_renderUs = 0;
}

// ---- bring-up console -------------------------------------------------
// Type a digit on the serial port to switch a redraw group off/on. This is
// how the render cost of each part of the dashboard gets measured on the
// real panel, and it stays useful for later tuning.
uint16_t g_mask = 0xFFFF;
uint8_t g_pclkStep = 0;  // index into the 'p' command's clock table (boot value = 24 MHz)

void handleSerial() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    switch (c) {
      case '1': g_mask ^= 0x0001; break;  // zone arcs
      case '2': g_mask ^= 0x0002; break;  // needle
      case '3': g_mask ^= 0x0004; break;  // ticks + numbers
      case '4': g_mask ^= 0x0008; break;  // rpm digits + shift lamp
      case '5': g_mask ^= 0x0010; break;  // tile readouts
      case '6': g_mask ^= 0x0020; break;  // status bar
      case '0': g_mask = 0xFFFF; break;   // everything on
      case '9': g_mask = 0x0000; break;   // everything off
      case 'p': {
        // Step the panel's pixel clock. The panel reads its frame buffer
        // out of PSRAM at ~2 bytes per PCLK, so this is a direct dial on
        // the DMA's PSRAM appetite: if the picture stops jittering at a
        // lower clock, the DMA was being starved of bandwidth.
        static const uint32_t kSteps[] = {24000000, 20000000, 16000000, 12000000};
        g_pclkStep = (g_pclkStep + 1) % (sizeof(kSteps) / sizeof(kSteps[0]));
        const uint32_t hz = kSteps[g_pclkStep];
        const bool ok = crowpanel::setPclk(hz);
        Serial.printf("[dbg] pclk -> %u Hz (%s), refresh ~%u Hz\n", (unsigned)hz,
                      ok ? "accepted" : "REJECTED", (unsigned)crowpanel::refreshHzNow());
        break;
      }
      case 'd':
        Serial.printf("[dump] frame the panel is scanning -> %dx%d cells\n", LCD_H_RES, LCD_V_RES);
        crowpanel::dumpFrame(120, 36);
        break;
      case 'h':
        Serial.println("1=arcs 2=needle 3=ticks 4=digits 5=readouts 6=status 0=all 9=none p=pclk d=dump");
        break;
      default: continue;
    }
    if (c == 'p' || c == 'd') continue;  // clock/dump are not redraw-mask toggles
    ui::setDebugMask(g_mask);
    Serial.printf("[dbg] mask=0x%04X\n", g_mask);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(400);
  printBanner();

  cfg::begin();  // NVS settings first, the panel does not need them

  const bool displayOk = crowpanel::begin();
  if (displayOk) {
    ui::begin();
  } else {
    // No panel means no display and no touch. Keep running so the serial
    // console stays useful and the network side can still be diagnosed:
    // LVGL has no display object, and touching it would crash.
    Serial.println("[main] display bring-up FAILED - running headless");
  }

  telemetry::begin();   // network task on core 0

  Serial.println("[main] ready");
}

void loop() {
  const uint32_t now = millis();
  handleSerial();

  if ((uint32_t)(now - g_lastUiMs) >= kUiPeriodMs) {
    g_lastUiMs = now;
    ui::tick();
  }

  const uint32_t t0 = micros();
  const uint32_t next = lv_timer_handler();
  g_renderUs += micros() - t0;
  g_frames++;

  if ((uint32_t)(now - g_lastStatsMs) >= 5000) {
    g_lastStatsMs = now;
    printStats();
  }

  // Sleep until LVGL wants to run again, but never longer than one panel
  // frame and never zero (the idle task still needs to run).
  uint32_t waitMs = next < 1 ? 1 : (next > 20 ? 20 : next);
  delay(waitMs);
}
