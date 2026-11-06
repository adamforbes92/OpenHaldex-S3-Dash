// =====================================================================
// datafeed.cpp - merge ESP-NOW and HTTP into the snapshot the UI draws.
// =====================================================================
#include "net/datafeed.h"

#include <Arduino.h>
#include <math.h>

#include "net/ohx_now.h"

namespace feed {
namespace {

using ohxnow::Status;

// The last source we logged, so a transition is printed exactly once.
Source g_lastLogged = Source::None;
uint32_t g_lastChangeMs = 0;
uint32_t g_changes = 0;

/// ESP-NOW wins when it has a number; otherwise keep the HTTP value.
inline float pick(const float packet, const float http) {
  return isnan(packet) ? http : packet;
}

inline int pickInt(const float packet, const int http) {
  return isnan(packet) ? http : (int)lroundf(packet);
}

const char *label(const Source s) {
  switch (s) {
    case Source::EspNow: return "esp-now";
    case Source::Http: return "http";
    default: return "none";
  }
}

}  // namespace

uint32_t nowAgeMs() { return ohxnow::stats().ageMs; }

bool onNow() {
  const uint32_t age = nowAgeMs();
  return age != UINT32_MAX && age <= kHoldMs;
}

Source source() {
  if (onNow()) return Source::EspNow;
  // "HTTP" only once the poll has actually produced something; before that
  // the honest answer is "nothing has arrived yet".
  return telemetry::live().ageMs == UINT32_MAX ? Source::None : Source::Http;
}

const char *sourceName() { return label(source()); }

void poll() {
#if OHX_USE_ESP_NOW
  ohxnow::poll();
#endif

  const Source s = source();
  if (s != g_lastLogged) {
    g_lastLogged = s;
    g_changes++;
    Serial.printf("[feed] values now come from %s (change %lu)\n", label(s),
                  (unsigned long)g_changes);
  }
}

telemetry::Live live() {
  const telemetry::Live http = telemetry::live();
  if (!onNow()) return http;

  const Status p = ohxnow::last();
  telemetry::Live v = http;  // keeps coolant and engine oil, which the packet does not carry
  v.rpm = pick(p.rpm, http.rpm);
  v.speed = pick(p.speed, http.speed);
  v.throttle = pick(p.throttle, http.throttle);
  v.boost = pick(p.boost, http.boost);
  // Not the packet's oilTemp: that is the Haldex's own oil (Gen4, over KWP), and
  // the OIL tile is engine oil from Kombi_2 with engine thresholds on it. They
  // are different fluids, so one is no fallback for the other. v.oil stays HTTP.
  v.lockTarget = pick(p.lockTarget, http.lockTarget);
  v.lockActual = pick(p.lockActual, http.lockActual);

  // Report the age of whichever sample the fields above actually came from,
  // so the console's "live data age" stays truthful.
  const uint32_t nowAge = nowAgeMs();
  v.ageMs = nowAge < http.ageMs ? nowAge : http.ageMs;
  v.everOk = true;
  return v;
}

telemetry::Dash dash() {
  const telemetry::Dash http = telemetry::dash();
  if (!onNow()) return http;

  const Status p = ohxnow::last();
  telemetry::Dash v = http;  // keeps tcOn / asrOn / awdWarning, HTTP only
  v.mode = (int)p.mode;      // always a real byte in the packet
  v.haldexState = (int)p.haldexState;
  v.lockTarget = pickInt(p.lockTarget, http.lockTarget);
  v.lockActual = pickInt(p.lockActual, http.lockActual);
  v.chassisCAN = p.chassisCan;
  v.haldexCAN = p.haldexCan;
  v.brakeIn = p.brake;
  v.handbrakeIn = p.handbrake;
  v.couplingOpen = p.couplingOpen;
  v.tempProtection = p.tempProtection;
  v.everOk = true;
  return v;
}

bool fresh(uint32_t staleMs) {
  const uint32_t age = nowAgeMs();
  if (age != UINT32_MAX && age <= staleMs) return true;
  return telemetry::liveFresh(staleMs);
}

}  // namespace feed
