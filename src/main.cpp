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
#include "net/ohx_now.h"
#include "net/datafeed.h"
#include "app/capability.h"
#include "app/ota.h"
#include "version.h"
#include "app/config.h"
#include "ui/dash_ui.h"
#include "ui/raw_ui.h"

namespace {

// UI refresh ceiling. The data arrives at 10 Hz; rendering faster than
// ~30 fps only burns CPU, because the RGB panel scans out at ~33 fps.
constexpr uint32_t kUiPeriodMs = 33;

uint32_t g_lastUiMs = 0;
uint32_t g_lastStatsMs = 0;
uint32_t g_frames = 0;
uint32_t g_renderUs = 0;
uint32_t g_busyUs = 0;   // the whole loop body, so the sleep cannot hide anything

// esp_reset_reason() in words. The panel and the WiFi share a 5 V rail with
// very little margin on a USB port, so "brownout" and "panic" have to be told
// apart from the console rather than guessed at.
const char *resetReasonText() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "PANIC (crash)";
    case ESP_RST_INT_WDT: return "watchdog: interrupt";
    case ESP_RST_TASK_WDT: return "watchdog: task";
    case ESP_RST_WDT: return "watchdog: other";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT (supply sagged)";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
  }
}

void printBanner() {
  Serial.println();
  // The prebuilt IDF libraries carry their own configuration; these values
  // decide the memory bandwidth the RGB panel has to share, so print them
  // rather than assume.
#ifdef CONFIG_SPIRAM_MODE_OCT
  const char *psramMode = "octal";
#elif defined(CONFIG_SPIRAM_MODE_QUAD)
  const char *psramMode = "quad";
#else
  const char *psramMode = "?";
#endif
#if defined(CONFIG_SPIRAM_SPEED_120M)
  const int psramMHz = 120;
#elif defined(CONFIG_SPIRAM_SPEED_80M)
  const int psramMHz = 80;
#elif defined(CONFIG_SPIRAM_SPEED_40M)
  const int psramMHz = 40;
#else
  const int psramMHz = 0;
#endif
  Serial.printf(" psram cfg: %s %d MHz", psramMode, psramMHz);
#ifdef CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
  Serial.print(", wifi/lwip buffers in PSRAM");
#endif
#ifdef CONFIG_LCD_RGB_RESTART_IN_VSYNC
  Serial.print(", rgb restart in vsync");
#endif
  Serial.println();
  Serial.println("=========================================");
  Serial.println(" OpenHaldex S3 Dash");
  Serial.printf(" version  : %s (%s)\n", OHX_DASH_VERSION, OHX_DASH_BUILD);
  Serial.printf(" board    : %s\n", BOARD_NAME);
  Serial.printf(" chip     : %s rev %d, %d core(s) @ %u MHz\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), getCpuFrequencyMhz());
  Serial.printf(" flash    : %u MB\n", (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)));
  Serial.printf(" psram    : %u MB free of %u MB\n",
                (unsigned)(ESP.getFreePsram() / (1024 * 1024)),
                (unsigned)(ESP.getPsramSize() / (1024 * 1024)));
  Serial.printf(" heap     : %u B free\n", (unsigned)ESP.getFreeHeap());
  Serial.printf(" sdk      : %s\n", ESP.getSdkVersion());
  // Why the chip restarted, which is the first question when the dash
  // reboots in the car and there is nobody watching the console. A brownout
  // and a panic look identical from the driver's seat.
  Serial.printf(" reset    : %s\n", resetReasonText());
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
  if (ui::staleEvents() > 0) {
    Serial.printf("[stats] live data went stale %lu time(s), oldest data %lu ms\n",
                  (unsigned long)ui::staleEvents(), (unsigned long)ui::maxStaleAgeMs());
  }
  // Which transport the numbers on the glass are coming from. Worth one line:
  // a silent switch to the HTTP fallback looks like nothing at all on screen.
  {
    const uint32_t nowAge = feed::nowAgeMs();
    char ageTxt[16];
    if (nowAge == UINT32_MAX) {
      snprintf(ageTxt, sizeof(ageTxt), "never");
    } else {
      snprintf(ageTxt, sizeof(ageTxt), "%lu ms", (unsigned long)nowAge);
    }
    Serial.printf("[stats] source %s (packet age %s, http %s)\n", feed::sourceName(), ageTxt,
                  s.wifiUp ? "up" : "down");
  }
#if OHX_USE_ESP_NOW
  {
    const ohxnow::Stats n = ohxnow::stats();
    if (n.enabled) {
      Serial.printf("[stats] esp-now %lu pkt (%.1f Hz), age %s, rej %lu dup %lu gap %lu, "
                    "ch %u, rssi %d dBm\n",
                    (unsigned long)n.packets, (double)n.hz,
                    n.ageMs == UINT32_MAX ? "never" : "live", (unsigned long)n.rejects,
                    (unsigned long)n.duplicates, (unsigned long)n.gaps, (unsigned)n.channel,
                    (int)n.rssi);
    }
  }
#endif
  const ui::Timings t = ui::takeTimings();
  const uint32_t flushUs = crowpanel::takeFlushMicros();
  const uint32_t waitUs = crowpanel::takeWaitMicros();
  const uint32_t vsyncs = crowpanel::takeVsyncCount();
  const uint32_t n = t.calls ? t.calls : 1;
  Serial.printf("[stats] ms/tick: gauge %.2f readouts %.2f conn %.2f  (mask 0x%04X)\n",
                (t.gaugeUs / 1000.0f) / n, (t.readoutsUs / 1000.0f) / n,
                (t.connUs / 1000.0f) / n, ui::debugMask());

  // Where the touch samples actually land, and how steady the press is. A
  // mirrored axis shows up as a point in the wrong corner; a flickering press
  // shows up as a large "edges" count (a finger held down should be one edge).
  const crowpanel::TouchProbe tp = crowpanel::touchProbe();
  if (tp.count) {
    Serial.printf("[stats] touch %lu pts, %lu edges, read<= %.2f ms, x %ld..%ld y %ld..%ld, "
                  "last %ld,%ld %s\n",
                  (unsigned long)tp.count, (unsigned long)tp.edges, tp.maxReadUs / 1000.0f,
                  (long)tp.minX, (long)tp.maxX, (long)tp.minY, (long)tp.maxY, (long)tp.lastX,
                  (long)tp.lastY, tp.lastPressed ? "down" : "up");
  }
  // "ms/loop" is the render only; "ms/busy" is the whole loop body minus the
  // sleep. They should be the same number: if they ever drift apart, something
  // outside LVGL is blocking.
  Serial.printf(
      "[stats] render %.1f fps | %.2f ms/loop, %.2f ms/busy of a %.1f ms period | "
      "%.2f ms/flush (%.2f ms/wait) | vsync %.0f/s | heap %u B, psram %u B\n",
      g_frames * 1000.0f / 5000.0f, (g_renderUs / 1000.0f) / (g_frames ? g_frames : 1),
      (g_busyUs / 1000.0f) / (g_frames ? g_frames : 1), 5000.0f / (g_frames ? g_frames : 1),
      (flushUs / 1000.0f) / (g_frames ? g_frames : 1),
      (waitUs / 1000.0f) / (g_frames ? g_frames : 1), vsyncs / 5.0f,
      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
  g_busyUs = 0;
  g_renderUs = 0;
  g_frames = 0;
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
      case '1': g_mask ^= 0x0001; break;  // rev bar
      case '2': g_mask ^= 0x0002; break;  // rev tick numbers
      case '3': g_mask ^= 0x0004; break;  // big rev number
      case '4': g_mask ^= 0x0008; break;  // tile readouts
      case '5': g_mask ^= 0x0010; break;  // status bar
      case '0': g_mask = 0xFFFF; break;   // everything on
      case '9': g_mask = 0x0000; break;   // everything off
      case 'r':
        // Which way the GT911's axes map onto the panel. Wrong here and every
        // press lands on some other widget: taps do nothing while a long
        // press, which is on a huge target, still works.
        Serial.printf("[dbg] touch rotation -> %s\n", crowpanel::cycleTouchRotation());
        break;
      case 't':
        crowpanel::resetTouchProbe();
        Serial.println("[dbg] touch probe reset");
        break;
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
      case 'b': {
        // 0 lines = the RGB DMA reads PSRAM directly (Elecrow's own path);
        // 16 = the driver prefetches into internal SRAM.
        const uint32_t next = crowpanel::bounceLines() ? 0 : 16;
        Serial.printf("[dbg] rebuilding panel with bounce=%u lines ...\n", (unsigned)next);
        const bool ok = crowpanel::setBounceLines(next);
        Serial.printf("[dbg] bounce=%u %s, flush %.2f ms\n", (unsigned)crowpanel::bounceLines(),
                      ok ? "ok" : "FAILED", (double)crowpanel::takeFlushMicros() / 1000.0);
        continue;
      }
      case 'd':
        Serial.printf("[dump] frame the panel is scanning -> %dx%d cells\n", LCD_H_RES, LCD_V_RES);
        crowpanel::dumpFrame(120, 36);
        break;
      case 'n':
        // Pause/resume the HTTP polling with the link still up: if the frame
        // rate recovers, our own requests are the cost; if it does not, the
        // radio receiving the rest of the channel is.
        {
          static bool paused = false;
          paused = !paused;
          telemetry::setPaused(paused);
        }
        continue;
      case 'h':
        Serial.println("1=arcs 2=needle 3=ticks 4=digits 5=readouts 6=status 0=all 9=none p=pclk b=bounce d=dump n=polling e=esp-now f=forget controller v=tile band x=raw page m=mode o=ota O=ota-fail-test");
        break;
      case 'v': {
        // Force the tile band, for bench work: the capability decision is made
        // once at boot, and this is how the other band is seen without
        // pretending to be another car.
        const layout::Variant next = (ui::layoutVariant() == layout::Variant::Full)
                                         ? layout::Variant::NoBoost
                                         : layout::Variant::Full;
        ui::applyLayout(next);
        continue;
      }
      case 'x':
        // The raw telemetry page: every value the dash can see, over both
        // transports, with absent fields shown as "--".
        rawui::toggle();
        continue;
      case 'm':
        // The mode switch, from the console: identical to tapping the HALDEX
        // panel, so the send path can be exercised without touching the glass.
        ui::cycleHaldexMode();
        continue;
      case 'O':
        // Bench aid: arm the next boot as a failing trial, so the over-the-air
        // fall-back path is exercised for real. Refused when the other slot is
        // empty - the point is to test the recovery, not the bootloader.
        ota::armFailureTest(15000);
        continue;
      case 'o':
        Serial.printf("[ota] %s | page %s\n", ota::stateLine(), ota::url());
        continue;
      case 'f':
        // Forget the remembered controller: how a replaced one gets picked up.
        ohxnow::forgetController();
        continue;
      case 'e':
#if OHX_USE_ESP_NOW
        ohxnow::printCompare();
#else
        Serial.println("[now] built without OHX_USE_ESP_NOW");
#endif
        continue;
      case 'E':
#if OHX_USE_ESP_NOW
        // A/B on one boot: does listening cost the render rate?
        {
          const bool on = !ohxnow::enabled();
          Serial.printf("[now] %s listener ...\n", on ? "enabling" : "disabling");
          ohxnow::setEnabled(on);
        }
#else
        Serial.println("[now] built without OHX_USE_ESP_NOW");
#endif
        continue;
      default: continue;
    }
    if (c == 'p' || c == 'd' || c == 'b') continue;  // clock/bounce/dump are not mask toggles
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
  ohxnow::loadController();  // which controller is ours, before the network task joins one

  // Before anything else that can fail: if this image is on trial and has
  // already failed, this is where the car is handed back to the previous
  // slot. It does nothing at all in the normal case.
  ota::begin();

  const bool displayOk = crowpanel::begin();
  if (displayOk) {
    ui::begin();
    rawui::begin();  // the raw telemetry page, hidden until asked for
  } else {
    // No panel means no display and no touch. Keep running so the serial
    // console stays useful and the network side can still be diagnosed:
    // LVGL has no display object, and touching it would crash.
    Serial.println("[main] display bring-up FAILED - running headless");
  }

  // Measured with the panel already scanning, which is the number that matters.
  crowpanel::psramBandwidthProbe();
  telemetry::begin();   // network task on core 0

  Serial.println("[main] ready");
}

void loop() {
  const uint32_t now = millis();
  const uint32_t loopStart = micros();
  handleSerial();

  // Pumps the ESP-NOW listener (a no-op if this build has it switched off)
  // and logs any change of the source the glass is being fed from.
  feed::poll();

  // The over-the-air trial clock: does nothing unless an update is waiting to
  // prove itself, in which case this is what confirms it or gives up on it.
  ota::tick();

  // What the car can fill, and the dashboard's shape, decided once. The
  // capability poll does nothing at all once it has settled; the layout is
  // applied only on the transition, so the console can then force a band by
  // hand without the decision immediately overwriting it.
  cap::poll();
  {
    static bool layoutApplied = false;
    const cap::Caps &caps = cap::get();
    if (caps.settled && !layoutApplied) {
      layoutApplied = true;
      ui::applyLayout(caps.variant);
    }
  }

  if ((uint32_t)(now - g_lastUiMs) >= kUiPeriodMs) {
    g_lastUiMs = now;
    // The raw page is refreshed first: while it is up the dashboard tick
    // returns immediately, so this is the only thing drawing.
    rawui::tick();
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
  g_busyUs += micros() - loopStart;
  uint32_t waitMs = next < 1 ? 1 : (next > 20 ? 20 : next);
  delay(waitMs);
}
