// =====================================================================
// datafeed.cpp - merge ESP-NOW and HTTP into the snapshot the UI draws.
// =====================================================================
#include "net/datafeed.h"

#include <Arduino.h>
#include <math.h>

#include "app/config.h"
#include "net/can_relay.h"
#include "net/ohx_now.h"

namespace feed {
namespace {

using ohxnow::Status;

constexpr uint32_t kLiveSlowMs = 5000;  // /api/live while it only has temperatures to add
constexpr uint32_t kDashSlowMs = 2000;  // /api/dashboard while it only has the AWD lamp to add

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
  canrelay::poll();
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
  telemetry::Live v = http;  // keeps coolant and engine oil unless the relay has them
  v.rpm = pick(p.rpm, http.rpm);
  v.speed = pick(p.speed, http.speed);
  v.throttle = pick(p.throttle, http.throttle);
  v.boost = pick(p.boost, http.boost);
  // Not the packet's oilTemp: that is the Haldex's own oil (Gen4, over KWP), and
  // the OIL tile is engine oil from Kombi_2 with engine thresholds on it. They
  // are different fluids, so one is no fallback for the other.
  const canrelay::Values relay = canrelay::values(p.generation);
  if (!isnan(relay.oil)) v.oil = relay.oil;
  if (!isnan(relay.coolant)) v.coolant = relay.coolant;
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
  telemetry::Dash v = http;  // keeps awdWarning, which only HTTP has
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
  // The API works these out from the same two flags: on = not forced off, and
  // null (so off here) without chassis CAN.
  v.tcOn = p.chassisCan && !p.tcForce;
  v.asrOn = p.chassisCan && !p.asrOff;
  const canrelay::Values relay = canrelay::values(p.generation);
  if (relay.brake >= 0) v.brakeFromCAN = relay.brake;
  if (relay.handbrake >= 0) v.handbrakeFromCAN = relay.handbrake;
  v.everOk = true;
  return v;
}

uint32_t livePeriodMs() {
  const uint32_t normal = cfg::get().pollLiveMs;
  if (!onNow()) return normal;
  if (!isnan(canrelay::values(ohxnow::last().generation).coolant)) return 0;
  return kLiveSlowMs;
}

uint32_t dashPeriodMs() {
  const uint32_t normal = cfg::get().pollDashMs;
  if (!onNow()) return normal;
  const canrelay::Values relay = canrelay::values(ohxnow::last().generation);
  if (relay.brake >= 0 && relay.handbrake >= 0 && normal < kDashSlowMs) return kDashSlowMs;
  return normal;
}

bool fresh(uint32_t staleMs) {
  const uint32_t age = nowAgeMs();
  if (age != UINT32_MAX && age <= staleMs) return true;
  return telemetry::liveFresh(staleMs);
}

}  // namespace feed
