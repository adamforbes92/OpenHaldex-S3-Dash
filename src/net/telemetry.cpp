// =====================================================================
// telemetry.cpp - WiFi link + the two HTTP pollers
// =====================================================================
#include "net/telemetry.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <cmath>
#include <cstring>

#include "app/config.h"
#include "app/ota.h"
#include "net/datafeed.h"
#include "net/ohx_now.h"

namespace telemetry {
namespace {

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

Live g_live;
Dash g_dash;
Stats g_stats;

// Written only by the network task; read by nobody else.
uint32_t g_liveOkMs = 0;
uint32_t g_dashOkMs = 0;

// One HTTPClient per endpoint: setReuse(true) keeps the TCP socket open, so
// 10 requests/second cost 10 request/response pairs on one connection rather
// than 10 handshakes. That matters because the C6 only allows
// CONFIG_LWIP_MAX_SOCKETS = 8 and TIME_WAIT churn would starve it.
HTTPClient g_httpLive;
HTTPClient g_httpDash;
bool g_httpLiveReady = false;
bool g_httpDashReady = false;

float numOrNan(JsonVariantConst v) {
  if (v.isNull()) return NAN;
  if (v.is<float>()) return v.as<float>();
  return NAN;
}

int intOrMinus1(JsonVariantConst v) {
  // The controller mixes the two: counts and states arrive as integers, but
  // brakeFromCAN / handbrakeFromCAN are declared `bool` in its own headers and
  // arrive as JSON true/false. Reading a boolean with is<int>() fails, so
  // every one of those read as "not present" and drew as "--". A value the
  // controller genuinely has no source for is a JSON null, and -1 is reserved
  // for that: "no data" and "off" must not look the same.
  if (v.is<bool>()) return v.as<bool>() ? 1 : 0;
  if (v.is<int>()) return v.as<int>();
  return -1;
}

// ------------------------------------------------------------- WiFi link
// Leaves the association, and leaves the radio on. Passing true as the first
// argument shuts the whole WiFi driver down (esp_wifi_stop + esp_wifi_deinit)
// with ESP-NOW still registered on top of it, which the IDF says must be undone
// first, and nothing here would ever have restarted the listener afterwards.
void wifiDisconnect() { WiFi.disconnect(false, false); }

// Last octet of the address used when the access point never leases one.
// High enough to stay clear of the DHCP pool's usual .2 ... .10 range.
constexpr uint8_t kStaticLastOctet = 203;

// Association state, which tells "on the radio but no address" apart from
// "never got near the AP". Asked of the driver directly: with the radio left on
// across a rejoin, WiFi.channel() is non-zero whether associated or not.
bool wifiAssociated() {
  wifi_ap_record_t ap;
  return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}
uint32_t wifiIp() { return (uint32_t)WiFi.localIP(); }

// The core reports WL_CONNECTED on GOT_IP; with a static address the netif
// can be up before that event, so both are accepted.
bool wifiLinkUp() { return WiFi.status() == WL_CONNECTED || (wifiAssociated() && wifiIp() != 0); }

// Wait for an address, reporting what the station is doing every 2 s.
bool waitForJoin(uint32_t timeoutMs) {
  static const char *const kStatus[] = {"IDLE",         "NO_SSID_AVAIL", "SCAN_COMPLETED",
                                        "CONNECTED",    "CONNECT_FAILED", "CONNECTION_LOST",
                                        "DISCONNECTED", "STOPPED"};
  const uint32_t deadline = millis() + timeoutMs;
  uint32_t lastLog = 0;
  while (!wifiLinkUp() && (int32_t)(millis() - deadline) < 0) {
    if ((uint32_t)(millis() - lastLog) >= 2000) {
      lastLog = millis();
      const int st = (int)WiFi.status();
      Serial.printf("[net]   status %d (%s) bssid=%s ch=%d rssi=%d ip=%s\n", st,
                    (st >= 0 && st <= 7) ? kStatus[st] : "?", WiFi.BSSIDstr().c_str(),
                    (int)WiFi.channel(), (int)WiFi.RSSI(),
                    WiFi.localIP().toString().c_str());
    }
    delay(50);
  }
  return wifiLinkUp();
}

bool wifiEnsureConnected() {
  static uint32_t lastAttempt = 0;
  static uint32_t lastRevision = 0;

  const uint32_t rev = cfg::revision();
  const bool revisionChanged = (rev != lastRevision);

  // Joined the right access point? Two cars can share the default SSID, and the
  // HTTP values must come from the same controller as the ESP-NOW ones. Checked
  // once a second, because this runs every 2 ms.
  static uint32_t lastApCheck = 0;
  static bool onWrongAp = false;
  if (!revisionChanged && wifiLinkUp()) {
    if ((uint32_t)(millis() - lastApCheck) >= 1000) {
      lastApCheck = millis();
      uint8_t joined[6];
      uint8_t ours[6];
      if (WiFi.BSSID(joined) != nullptr) {
        if (!ohxnow::controller(ours)) {
          ohxnow::setController(joined);  // nothing heard yet: the AP we joined is it
          onWrongAp = false;
        } else {
          onWrongAp = memcmp(joined, ours, 6) != 0;
          if (onWrongAp) Serial.println("[net] joined another controller's AP - rejoining ours");
        }
      }
    }
    if (!onWrongAp) return true;
  }
  onWrongAp = false;

  const uint32_t now = millis();
  // A bounced access point should be back within a couple of seconds. The
  // C6 briefly drops its own AP on a mode change, and a 10 s backoff on top
  // of the join turned that into half a minute of a stale dashboard.
  if (!revisionChanged && lastAttempt != 0 && (uint32_t)(now - lastAttempt) < 3000) {
    return false;  // back off: retry at most every 3 s
  }
  lastAttempt = now;
  lastRevision = rev;

  char ssid[33], pass[65], host[48];
  uint16_t port = 80;
  cfg::connectionCopy(ssid, sizeof(ssid), pass, sizeof(pass), host, sizeof(host), port);

  g_httpLiveReady = false;
  g_httpDashReady = false;
  g_httpLive.end();
  g_httpDash.end();

  WiFi.setAutoReconnect(false);  // we drive this loop ourselves
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // 10 Hz polling: latency beats power saving here
  wifiDisconnect();

  // The OpenHaldex-C6 access point is open by default. The core's default
  // minimum security can refuse an open network outright, which looks
  // exactly like "wrong password" from the outside.
  if (pass[0] == '\0') {
    WiFi.setMinSecurity(WIFI_AUTH_OPEN);
  }

  // Learned from an earlier join: DHCP never completes against this AP, so go
  // straight to a static address instead of spending 15 s finding out again.
  const bool preferStatic = cfg::get().netUseStatic != 0;
  IPAddress hostIp;
  // The radio now stays on between attempts, so a static address tried on an
  // earlier attempt is still set. It has to be taken back off by hand or DHCP
  // never gets another go.
  static bool staticInUse = false;
  if (preferStatic && hostIp.fromString(host)) {
    const IPAddress staticIp(hostIp[0], hostIp[1], hostIp[2], kStaticLastOctet);
    WiFi.config(staticIp, hostIp, IPAddress(255, 255, 255, 0));
    staticInUse = true;
    Serial.printf("[net] static %s (no DHCP on this access point)\n",
                  staticIp.toString().c_str());
  } else if (staticInUse) {
    WiFi.config(IPAddress(), IPAddress(), IPAddress());  // all zero = back to DHCP
    staticInUse = false;
  }

  // Aimed at our controller's own MAC when we know it, so a stronger access
  // point with the same SSID in the next parking space cannot pull us away.
  uint8_t ours[6];
  const uint8_t *bssid = ohxnow::controller(ours) ? ours : nullptr;
  Serial.printf("[net] joining \"%s\" (password %s%s)\n", ssid, pass[0] ? "set" : "empty",
                bssid ? ", our controller only" : "");
  WiFi.begin(ssid, pass[0] ? pass : nullptr, 0, bssid);

  bool up = waitForJoin(preferStatic ? 8000 : 15000);

  // Associated but never leased an address. That is what happens when the
  // OpenHaldex-C6 is also sitting on a home LAN that already uses
  // 192.168.1.0/24: its softAP DHCP server sees those addresses as taken and
  // the lease never completes, while the association itself looks perfect.
  // Falling back to a static address in the host's subnet skips DHCP
  // entirely, which is also what a fixed install in a car wants.
  if (!up && wifiAssociated()) {
    if (hostIp.fromString(host)) {
      const IPAddress staticIp(hostIp[0], hostIp[1], hostIp[2], kStaticLastOctet);
      Serial.printf("[net] associated but no DHCP lease - retrying with static %s\n",
                    staticIp.toString().c_str());
      if (WiFi.config(staticIp, hostIp, IPAddress(255, 255, 255, 0))) {
        staticInUse = true;
        WiFi.disconnect();
        delay(100);
        WiFi.begin(ssid, pass[0] ? pass : nullptr, 0, bssid);
        up = waitForJoin(8000);
        if (up && !preferStatic) {
          cfg::Data d = cfg::get();
          d.netUseStatic = 1;
          cfg::apply(d);
          lastRevision = cfg::revision();  // our own write, not a user edit
          Serial.println("[net] remembered: use a static address from now on");
        }
      } else {
        Serial.println("[net] WiFi.config() refused the static address");
      }
    } else {
      Serial.printf("[net] cannot derive a static address from host \"%s\"\n", host);
    }
  }

  taskENTER_CRITICAL(&g_mux);
  g_stats.reconnects++;
  taskEXIT_CRITICAL(&g_mux);

  if (!up) {
    const int st = (int)WiFi.status();
    Serial.printf("[net] join FAILED (status %d), retrying in 10 s\n", st);
    // Walking the channel list by hand finds an AP that the fast scan missed.
    const int n = WiFi.scanNetworks();
    if (n > 0) {
      Serial.printf("[net] %d AP(s) visible:", n);
      for (int i = 0; i < n && i < 6; i++) {
        Serial.printf(" [%s ch%d %ddBm]", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
      }
      Serial.println();
    } else {
      Serial.printf("[net] no APs visible at all (scan returned %d)\n", n);
    }
    WiFi.scanDelete();
    return false;
  }

  Serial.printf("[net] connected: ip %s gateway %s rssi %d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                (int)WiFi.RSSI());

  // Registering mDNS lets "openhaldex.local" resolve wherever the C6 is
  // published under that name instead of its access-point address.
  static bool mdnsStarted = false;
  if (!mdnsStarted) {
    mdnsStarted = MDNS.begin("ohx-dash");
  }
  return true;
}

// --------------------------------------------------------------- polling
void buildUrl(char *buf, size_t len, const char *path) {
  char ssid[33], pass[65], host[48];
  uint16_t port = 80;
  cfg::connectionCopy(ssid, sizeof(ssid), pass, sizeof(pass), host, sizeof(host), port);
  if (port == 80) {
    snprintf(buf, len, "http://%s%s", host, path);
  } else {
    snprintf(buf, len, "http://%s:%u%s", host, port, path);
  }
}

// Parses the body with no intermediate String allocation.
int fetchJson(HTTPClient &http, bool &ready, const char *path, uint16_t timeoutMs,
              JsonDocument &doc) {
  char url[128];
  buildUrl(url, sizeof(url), path);

  if (!ready) {
    http.setReuse(true);
    http.setConnectTimeout(timeoutMs);
    http.setTimeout(timeoutMs);
    http.setUserAgent("OpenHaldex-S3-Dash/0.1");
    ready = true;
  }

  if (!http.begin(url)) return -1;
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return code;
  }
  doc.clear();
  const DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) return -2;
  return code;
}

// A poll that fails is worth one line, not one per attempt: without this the
// serial console turns into a wall of text on a link that has gone bad, and
// the interesting part (which code, how long) gets lost.
void logPollFailure(const char *what, int code) {
  static uint32_t lastLog = 0;
  static int lastCode = 0;
  const uint32_t now = millis();
  if (code == lastCode && (uint32_t)(now - lastLog) < 5000) return;
  lastLog = now;
  lastCode = code;
  Serial.printf("[net] %s failed: %d (%s)\n", what, code,
                code == -1 ? "not connected"
                           : code == -2 ? "bad JSON"
                                        : code > 0 ? "HTTP status" : "transport");
}

void pollLive() {
  static JsonDocument doc;
  // A longer poll interval must not turn into a longer stall when the C6 is
  // unreachable: the net task has to get back round the loop to notice a
  // dropped link quickly, so the receive timeout is capped rather than simply
  // scaled with the period.
  uint16_t tmo = (uint16_t)(cfg::get().pollLiveMs * 3);
  if (tmo < 300) tmo = 300;
  if (tmo > 1200) tmo = 1200;
  const int code = fetchJson(g_httpLive, g_httpLiveReady, "/api/live", tmo, doc);
  const uint32_t now = millis();

  if (code == HTTP_CODE_OK) {
    Live v;
    v.rpm = numOrNan(doc["rpm"]);
    v.speed = numOrNan(doc["speed"]);
    v.throttle = numOrNan(doc["throttle"]);
    v.boost = numOrNan(doc["boost"]);
    v.coolant = numOrNan(doc["coolantTemp"]);
    v.oil = numOrNan(doc["oilTemp"]);
    v.lockTarget = numOrNan(doc["lockTarget"]);
    v.lockActual = numOrNan(doc["lockActual"]);
    v.everOk = true;
    v.ageMs = 0;
    g_liveOkMs = now;

    taskENTER_CRITICAL(&g_mux);
    g_live = v;
    g_stats.liveOk++;
    taskEXIT_CRITICAL(&g_mux);
  } else {
    logPollFailure("/api/live", code);
    taskENTER_CRITICAL(&g_mux);
    g_stats.liveFail++;
    taskEXIT_CRITICAL(&g_mux);
  }
}

void pollDash() {
  static JsonDocument doc;
  uint16_t tmo = cfg::get().pollDashMs;
  if (tmo < 800) tmo = 800;
  const int code = fetchJson(g_httpDash, g_httpDashReady, "/api/dashboard", tmo, doc);
  const uint32_t now = millis();

  if (code == HTTP_CODE_OK) {
    Dash v;
    v.mode = intOrMinus1(doc["mode"]);
    v.haldexState = intOrMinus1(doc["haldexState"]);
    v.lockTarget = intOrMinus1(doc["lockTarget"]);
    v.lockActual = intOrMinus1(doc["lockActual"]);
    v.chassisCAN = doc["chassisCAN"] | false;
    v.haldexCAN = doc["haldexCAN"] | false;
    v.brakeIn = doc["brakeIn"] | false;
    // The C6 exposes both an "in" and an "out" handbrake flag depending on
    // the platform; the browser reference ORs them.
    v.handbrakeIn = (doc["handbrakeIn"] | false) || (doc["handbrakeOut"] | false);
    v.tcOn = doc["tcOn"] | false;
    v.asrOn = doc["asrOn"] | false;
    v.awdWarning = doc["awdWarning"] | false;
    v.couplingOpen = doc["couplingOpen"] | false;
    v.tempProtection = doc["tempProtection"] | false;
    // The CAN view of the pedals, and the steering angle the JSON path
    // gated off for Gen1. Reported as null by the C6 when unknown.
    v.brakeFromCAN = intOrMinus1(doc["brakeFromCAN"]);
    v.handbrakeFromCAN = intOrMinus1(doc["handbrakeFromCAN"]);
    v.steeringAngle = numOrNan(doc["steeringAngle"]);
    v.everOk = true;
    v.ageMs = 0;
    g_dashOkMs = now;

    taskENTER_CRITICAL(&g_mux);
    g_dash = v;
    g_stats.dashOk++;
    taskEXIT_CRITICAL(&g_mux);
  } else {
    logPollFailure("/api/dashboard", code);
    taskENTER_CRITICAL(&g_mux);
    g_stats.dashFail++;
    taskEXIT_CRITICAL(&g_mux);
  }
}

// Set from the serial console: keeps the link up but stops asking for data,
// so the cost of our own requests can be separated from the radio having to
// receive everything else on the channel.
volatile bool g_paused = false;

void netTask(void *) {
  // Let USB enumeration and the panel come up first.
  vTaskDelay(pdMS_TO_TICKS(500));

  uint32_t lastLive = 0;
  uint32_t lastDash = 0;
  uint32_t lastPlan = 0;
  uint32_t livePeriod = cfg::get().pollLiveMs;
  uint32_t dashPeriod = cfg::get().pollDashMs;

  for (;;) {
    if (!wifiEnsureConnected()) {
      // Say so: the status light's "NO WIFI" reads this, and the block further
      // down that normally sets it is skipped while the link is down.
      taskENTER_CRITICAL(&g_mux);
      g_stats.wifiUp = false;
      taskEXIT_CRITICAL(&g_mux);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // The update page lives on the station's own address, so it can only be
    // served once there is one. Both calls are trivial when nothing is
    // happening, and both sit above the polling pause on purpose: a paused
    // dash must still be updatable.
    ota::serveBegin();
    ota::servePoll();

    if (g_paused) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    const uint32_t now = millis();

    // How often each request is worth making changes as the packets and the CAN
    // relay come and go. Measured from the last request rather than scheduled
    // ahead, so when the packets stop the HTTP fallback starts straight away
    // instead of waiting out a slow period.
    if ((uint32_t)(now - lastPlan) >= 100) {
      lastPlan = now;
      livePeriod = feed::livePeriodMs();
      dashPeriod = feed::dashPeriodMs();
    }
    if (livePeriod != 0 && (uint32_t)(now - lastLive) >= livePeriod) {
      pollLive();
      lastLive = millis();
    }
    if (dashPeriod != 0 && (uint32_t)(now - lastDash) >= dashPeriod) {
      pollDash();
      lastDash = millis();
    }

    const bool up = (WiFi.status() == WL_CONNECTED);
    const int16_t rssi = up ? (int16_t)WiFi.RSSI() : 0;
    const uint32_t ip = up ? (uint32_t)WiFi.localIP() : 0;
    taskENTER_CRITICAL(&g_mux);
    g_stats.wifiUp = up;
    g_stats.rssi = rssi;
    g_stats.ip = ip;
    taskEXIT_CRITICAL(&g_mux);

    // 2 ms granularity keeps the 100 ms live tick accurate without spinning.
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

}  // namespace

void setPaused(bool paused) {
  g_paused = paused;
  Serial.printf("[net] polling %s\n", paused ? "PAUSED" : "resumed");
}

void begin() {
  g_live = Live{NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, false, UINT32_MAX};
  g_dash = Dash{-1, -1, -1, -1, false, false, false, false, false, false,
                false, false, false, false, UINT32_MAX, NAN, -1, -1};
  g_stats = Stats{};

  xTaskCreatePinnedToCore(netTask, "ohxnet", 10240, nullptr, 4, nullptr, 0);
}

Live live() {
  Live v;
  const uint32_t now = millis();
  taskENTER_CRITICAL(&g_mux);
  v = g_live;
  v.ageMs = g_liveOkMs ? (uint32_t)(now - g_liveOkMs) : UINT32_MAX;
  taskEXIT_CRITICAL(&g_mux);
  return v;
}

Dash dash() {
  Dash v;
  const uint32_t now = millis();
  taskENTER_CRITICAL(&g_mux);
  v = g_dash;
  v.ageMs = g_dashOkMs ? (uint32_t)(now - g_dashOkMs) : UINT32_MAX;
  taskEXIT_CRITICAL(&g_mux);
  return v;
}

Stats stats() {
  taskENTER_CRITICAL(&g_mux);
  Stats v = g_stats;
  taskEXIT_CRITICAL(&g_mux);
  return v;
}

bool liveFresh(uint32_t staleMs) { return g_liveOkMs != 0 && (millis() - g_liveOkMs) <= staleMs; }

void restart() {
  // The network task watches cfg::revision(): it drops the link and rejoins.
  cfg::requestReconnect();
}

}  // namespace telemetry
