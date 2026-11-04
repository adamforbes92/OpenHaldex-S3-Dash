// =====================================================================
// telemetry.cpp - WiFi link + the two HTTP pollers
// =====================================================================
#include "net/telemetry.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include <cmath>
#include <cstring>

#include "app/config.h"

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
  if (v.is<int>()) return v.as<int>();
  return -1;
}

// ------------------------------------------------------------- WiFi link
void wifiDisconnect() { WiFi.disconnect(true, false); }

bool wifiEnsureConnected() {
  static uint32_t lastAttempt = 0;
  static uint32_t lastRevision = 0;

  const uint32_t rev = cfg::revision();
  const bool revisionChanged = (rev != lastRevision);

  if (WiFi.status() == WL_CONNECTED && !revisionChanged) return true;

  const uint32_t now = millis();
  if (!revisionChanged && lastAttempt != 0 && (uint32_t)(now - lastAttempt) < 10000) {
    return false;  // back off: retry at most every 10 s
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

  Serial.printf("[net] joining \"%s\" (password %s)\n", ssid, pass[0] ? "set" : "empty");
  WiFi.begin(ssid, pass);

  const uint32_t deadline = millis() + 15000;
  while (WiFi.status() != WL_CONNECTED && (int32_t)(millis() - deadline) < 0) {
    delay(50);
  }

  taskENTER_CRITICAL(&g_mux);
  g_stats.reconnects++;
  taskEXIT_CRITICAL(&g_mux);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[net] join FAILED - will retry");
    return false;
  }

  Serial.printf("[net] connected: ip %s rssi %d dBm\n", WiFi.localIP().toString().c_str(),
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

void pollLive() {
  static JsonDocument doc;
  uint16_t tmo = (uint16_t)(cfg::get().pollLiveMs * 3);
  if (tmo < 300) tmo = 300;
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
    v.everOk = true;
    v.ageMs = 0;
    g_dashOkMs = now;

    taskENTER_CRITICAL(&g_mux);
    g_dash = v;
    g_stats.dashOk++;
    taskEXIT_CRITICAL(&g_mux);
  } else {
    taskENTER_CRITICAL(&g_mux);
    g_stats.dashFail++;
    taskEXIT_CRITICAL(&g_mux);
  }
}

void netTask(void *) {
  // Let USB enumeration and the panel come up first.
  vTaskDelay(pdMS_TO_TICKS(500));

  uint32_t nextLive = 0;
  uint32_t nextDash = 0;

  for (;;) {
    if (!wifiEnsureConnected()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    const uint32_t now = millis();

    if ((int32_t)(now - nextLive) >= 0) {
      pollLive();
      nextLive = millis() + cfg::get().pollLiveMs;
    }
    if ((int32_t)(now - nextDash) >= 0) {
      pollDash();
      nextDash = millis() + cfg::get().pollDashMs;
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

void begin() {
  g_live = Live{NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, false, UINT32_MAX};
  g_dash = Dash{-1, -1, -1, -1, false, false, false, false, false, false,
                false, false, false, false, UINT32_MAX};
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
  // Re-applying bumps cfg::revision(), which the network task watches: it
  // drops the link and reconnects with the new SSID / host.
  cfg::Data d = cfg::get();
  cfg::apply(d);
}

}  // namespace telemetry
