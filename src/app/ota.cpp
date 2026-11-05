// =====================================================================
// ota.cpp - see ota.h for what this is for and why it is built this way.
// =====================================================================
#include "app/ota.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "hal/crowpanel.h"
#include "version.h"

namespace ota {
namespace {

// The trial's own corner of NVS, deliberately not the settings blob.
constexpr const char *kNvs = "ohxota";
constexpr const char *kKeyTrial = "trial";  // 1 = this image has not proved itself
constexpr const char *kKeyBoots = "boots";  // boots taken with the trial armed
constexpr const char *kKeyFail = "fail";    // bench: how long this trial should live (ms)

// Three strikes. An image that has booted three times without standing down is
// not going to: a dashboard that works proves itself in twenty seconds.
constexpr uint8_t kMaxTrialBoots = 3;
// How long a trial gets to show a panel at all. The panel is up in about two
// seconds, so this is generous on purpose - giving up early on a slow but
// working image is its own kind of failure.
constexpr uint32_t kPanelDeadlineMs = 90000;
// How long the panel must stay up before the image is called good.
constexpr uint32_t kPanelHoldMs = 20000;

Preferences g_nvs;
bool g_trial = false;
uint32_t g_failAfterMs = 0;  // non-zero: bench run, fail this trial on purpose
uint32_t g_upSinceMs = 0;    // when the panel came up (0 = not yet)
uint32_t g_bootMs = 0;
uint8_t g_bootCount = 0;

WebServer g_server(80);
bool g_serving = false;
volatile bool g_busy = false;
volatile size_t g_written = 0;
volatile uint32_t g_rebootAtMs = 0;  // set by a successful upload
bool g_uploadOk = false;             // the last upload finished and verified
char g_line[96] = "ota: waiting";
char g_url[48] = "--";

// ------------------------------------------------------------- the way back
const esp_partition_t *otherSlot() { return esp_ota_get_next_update_partition(nullptr); }

// Does a slot hold something that looks like an app image? Used only to refuse
// a bench test that would fall back onto an empty partition; in the field the
// bootloader's own verification is what matters.
bool slotLooksFlashed(const esp_partition_t *p) {
  if (!p) return false;
  uint8_t magic = 0;
  if (esp_partition_read(p, 0, &magic, 1) != ESP_OK) return false;
  return magic == 0xE9;  // the first byte of every ESP image
}

void clearTrial() {
  g_trial = false;
  g_failAfterMs = 0;
  g_nvs.putUChar(kKeyTrial, 0);
  g_nvs.putUChar(kKeyBoots, 0);
  g_nvs.putULong(kKeyFail, 0);
}

// Give the car back. This is the whole safety net: point the bootloader at the
// other slot and reboot into it. The trial is cleared first, so an image that
// is failing yet boots far enough to run this cannot loop here.
void revertToOtherSlot(const char *why) {
  const esp_partition_t *other = otherSlot();
  if (!other) {
    Serial.printf("[ota] %s, but there is no other slot to fall back to\n", why);
    return;
  }
  Serial.printf("[ota] %s - falling back to %s and rebooting\n", why, other->label);
  const esp_err_t e = esp_ota_set_boot_partition(other);
  if (e != ESP_OK) {
    Serial.printf("[ota] could not select %s: %d (leaving the boot slot alone)\n",
                  other->label, (int)e);
    return;
  }
  clearTrial();
  g_nvs.end();  // let the write settle before the reset
  Serial.flush();
  delay(150);
  esp_restart();
}

// ------------------------------------------------------------- the way in
const char kPageHead[] PROGMEM =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>OpenHaldex S3 Dash</title><style>"
    "body{font-family:system-ui,-apple-system,sans-serif;background:#0e1116;color:#e8edf2;"
    "margin:0;padding:24px;line-height:1.5}h1{font-size:19px;margin:0 0 4px}"
    "h2{font-size:14px;color:#7c8894;font-weight:600;margin:22px 0 6px;text-transform:uppercase}"
    "code{color:#7fd1ff}table{border-collapse:collapse;margin:6px 0 4px}"
    "td{padding:3px 16px 3px 0;color:#c6ced6}"
    "td:first-child{color:#7c8894}button{padding:11px 20px;margin-top:16px;font-size:16px;"
    "border:0;border-radius:5px;background:#22c55e;color:#06140b;font-weight:700}"
    "input{font-size:15px}p{color:#aab4bf;max-width:44em}"
    ".warn{color:#f59e0b}</style></head><body>";

void sendPage(int code, const String &body) {
  g_server.sendHeader("Cache-Control", "no-store");
  g_server.send(code, "text/html", String(FPSTR(kPageHead)) + body + "</body></html>");
}

String factsHtml() {
  String s = "<h1>OpenHaldex S3 Dash</h1>";
  s += "<table><tr><td>Firmware</td><td><code>";
  s += OHX_DASH_VERSION;
  s += "</code> (";
  s += OHX_DASH_BUILD;
  s += ")</td></tr><tr><td>Running from</td><td><code>";
  s += runningSlot();
  s += "</code> ";
  s += (g_trial ? "<span class='warn'>on trial</span>" : "confirmed");
  s += "</td></tr><tr><td>Other slot</td><td><code>";
  const esp_partition_t *o = otherSlot();
  s += o ? o->label : "--";
  s += "</code>";
  if (!slotLooksFlashed(o)) s += " <span class='warn'>(no image: nothing to fall back to yet)</span>";
  s += "</td></tr></table>";
  return s;
}

void handleRoot() {
  if (g_busy) {
    sendPage(503, String("<h1>Updating</h1><p>An upload is already being written (") +
                      String((unsigned)g_written) + " bytes so far).</p>");
    return;
  }
  String s = factsHtml();
  s += "<h2>Update</h2><p>Choose the <code>firmware.bin</code> built for <b>this board</b> "
       "(CrowPanel 7.0\" ESP32-S3, 4 MB flash, the two-slot OTA layout). Nothing is written to "
       "flash until the upload has finished arriving, and the image boots <b>on trial</b>: if it "
       "does not come up and keep running, this one comes back by itself.</p>"
       "<form method='post' action='/update' enctype='multipart/form-data'>"
       "<input type='file' name='firmware' accept='.bin'><br>"
       "<button type='submit'>Upload and reboot</button></form>";
  sendPage(200, s);
}

// The upload's data callback. Chunks arrive here; Update writes them straight
// to the inactive slot, so a half-finished upload cannot damage the running
// image.
void handleUpload() {
  HTTPUpload &u = g_server.upload();
  if (u.status == UPLOAD_FILE_START) {
    g_busy = true;
    g_written = 0;
    g_uploadOk = false;
    Serial.printf("[ota] upload starting: %s\n", u.filename.c_str());
    // The slot is chosen by the IDF's own OTA target, which is the inactive
    // partition. UPDATE_SIZE_UNKNOWN because the browser's length is advisory.
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Serial.println("[ota] Update.begin refused");
      Update.printError(Serial);
    }
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (Update.write(u.buf, u.currentSize) != u.currentSize) {
      Update.printError(Serial);
    }
    g_written += u.currentSize;
  } else if (u.status == UPLOAD_FILE_END) {
    g_uploadOk = Update.end(true);
    if (!g_uploadOk) Update.printError(Serial);
    Serial.printf("[ota] upload finished: %u bytes, %s\n", (unsigned)g_written,
                  g_uploadOk ? "written" : "FAILED");
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    g_busy = false;
    Serial.println("[ota] upload aborted by the client");
  }
}

void handleUploadDone() {
  if (!g_uploadOk) {
    g_busy = false;
    // Update's own wording is about the flash layer ("Decryption error" for
    // anything it cannot parse). Say what it means to the person holding the
    // phone: the wrong file, and nothing happened.
    const char *why = Update.errorString();
    if (why && strstr(why, "Decryption")) {
      why = "that file is not a firmware image for this board";
    }
    sendPage(500, String("<h1>Update failed</h1><p>Nothing was changed: the running image is "
                         "still the running image. <code>") +
                      String(why) + "</code></p>" + factsHtml());
    return;
  }
  // Arm the trial from *this* image, on its way out: the next boot has to
  // prove itself, and this is the record that says so.
  g_nvs.putUChar(kKeyTrial, 1);
  g_nvs.putUChar(kKeyBoots, 0);
  Serial.printf("[ota] %u bytes written to %s - rebooting into it on trial\n",
                (unsigned)g_written, otherSlot() ? otherSlot()->label : "?");
  sendPage(200, String("<h1>Written</h1><p>") + String((unsigned)g_written) +
                    " bytes written. The dash is rebooting into the new image now - it will "
                    "appear on trial, and it has 90 s to prove itself.</p>" + factsHtml());
  g_rebootAtMs = millis() + 800;  // long enough for the reply to be sent
}

// ------------------------------------------------------------- public API
bool panelUp() { return crowpanel::status().panelOk; }

}  // namespace

void begin() {
  g_bootMs = millis();
  g_nvs.begin(kNvs, false);
  g_trial = g_nvs.getUChar(kKeyTrial, 0) != 0;
  g_bootCount = g_nvs.getUChar(kKeyBoots, 0);
  g_failAfterMs = g_nvs.getULong(kKeyFail, 0);

  Serial.printf("[ota] %s in %s | %s | %s\n", OHX_DASH_VERSION, runningSlot(),
                g_trial ? "ON TRIAL" : "confirmed", slotLooksFlashed(otherSlot())
                                                          ? "fallback ready"
                                                          : "no fallback image");

  if (!g_trial) return;

  g_bootCount++;
  g_nvs.putUChar(kKeyBoots, g_bootCount);
  if (g_bootCount > kMaxTrialBoots) {
    revertToOtherSlot("the trial image keeps rebooting");
    return;
  }
  if (g_failAfterMs) {
    Serial.printf("[ota] trial boot %u of %u | BENCH: failing on purpose in %lu ms\n",
                  (unsigned)g_bootCount, (unsigned)kMaxTrialBoots, (unsigned long)g_failAfterMs);
  } else {
    Serial.printf("[ota] trial boot %u of %u | the panel has %lu s to come up\n",
                  (unsigned)g_bootCount, (unsigned)kMaxTrialBoots,
                  (unsigned long)(kPanelDeadlineMs / 1000));
  }
  snprintf(g_line, sizeof g_line, "ota: %s on trial", OHX_DASH_VERSION);
}

void tick() {
  if (!g_trial) return;
  const uint32_t up = millis() - g_bootMs;

  // A bench run that was told to fail: prove the recovery, not the firmware.
  if (g_failAfterMs) {
    if (up >= g_failAfterMs) revertToOtherSlot("bench test: this trial was told to fail");
    return;
  }

  if (panelUp()) {
    if (g_upSinceMs == 0) {
      g_upSinceMs = millis();
      Serial.println("[ota] panel is up - the trial clock starts");
    } else if ((uint32_t)(millis() - g_upSinceMs) >= kPanelHoldMs) {
      Serial.printf("[ota] %s came up and stayed up: standing the trial down\n", OHX_DASH_VERSION);
      clearTrial();
      snprintf(g_line, sizeof g_line, "ota: %s confirmed (%s)", OHX_DASH_VERSION, runningSlot());
      return;
    }
  }

  if (up >= kPanelDeadlineMs) revertToOtherSlot("the panel never came up");
}

void serveBegin() {
  if (g_serving) return;
  if (WiFi.status() != WL_CONNECTED) return;

  g_server.on("/", HTTP_GET, handleRoot);
  g_server.on("/update", HTTP_POST, handleUploadDone, handleUpload);
  g_server.onNotFound([]() { sendPage(404, String("<h1>Not found</h1>") + factsHtml()); });
  g_server.begin();
  g_serving = true;

  // mDNS is already up under "ohx-dash" (telemetry.cpp starts it for the
  // controller's address), so this only has to advertise the page: the same
  // name then reaches either device, and this one is the dash.
  if (MDNS.addService("http", "tcp", 80)) {
    Serial.println("[ota] update page: http://ohx-dash.local/");
  }
  snprintf(g_url, sizeof g_url, "http://%s/", WiFi.localIP().toString().c_str());
  Serial.printf("[ota] update page listening on %s\n", g_url);
  snprintf(g_line, sizeof g_line, "ota: %s (%s)", OHX_DASH_VERSION, g_url);
}

void servePoll() {
  if (!g_serving) return;
  g_server.handleClient();

  if (g_rebootAtMs && (int32_t)(millis() - g_rebootAtMs) >= 0) {
    g_rebootAtMs = 0;
    g_nvs.end();
    Serial.println("[ota] rebooting into the new image");
    Serial.flush();
    delay(50);
    esp_restart();
  }
}

bool busy() { return g_busy; }
size_t written() { return g_written; }
bool onTrial() { return g_trial; }

const char *runningSlot() {
  const esp_partition_t *p = esp_ota_get_running_partition();
  return p ? p->label : "?";
}

const char *url() { return g_url; }

const char *stateLine() {
  static char line[96];
  snprintf(line, sizeof line, "ota: %s in %s%s", OHX_DASH_VERSION, runningSlot(),
           g_trial ? " (on trial)" : "");
  return line;
}

bool armFailureTest(uint32_t failAfterMs) {
  const esp_partition_t *other = otherSlot();
  if (!slotLooksFlashed(other)) {
    Serial.printf("[ota] refusing the bench test: %s holds no image, so there would be "
                  "nothing to fall back to\n",
                  other ? other->label : "the other slot");
    return false;
  }
  if (busy()) {
    Serial.println("[ota] refusing the bench test: an upload is in progress");
    return false;
  }
  Preferences nvs;
  nvs.begin(kNvs, false);
  nvs.putUChar(kKeyTrial, 1);
  nvs.putUChar(kKeyBoots, 0);
  nvs.putULong(kKeyFail, failAfterMs);
  nvs.end();
  Serial.printf("[ota] bench test armed: this boot goes on trial and will fall back to %s "
                "after %lu ms. Rebooting...\n",
                other->label, (unsigned long)failAfterMs);
  Serial.flush();
  delay(200);
  esp_restart();
  return true;  // not reached
}

}  // namespace ota
