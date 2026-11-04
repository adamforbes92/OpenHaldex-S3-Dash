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
  Serial.printf("[stats] render %.1f fps, %.1f ms/frame, heap %u B, psram %u B free\n",
                g_frames * 1000.0f / 5000.0f, (g_renderUs / 1000.0f) / (g_frames ? g_frames : 1),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  g_frames = 0;
  g_renderUs = 0;
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
