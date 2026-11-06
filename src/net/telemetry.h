// =====================================================================
// telemetry.h - the OpenHaldex-C6 client
//
// Two independent HTTP pollers run in a dedicated task on core 0:
//
// How often each one actually runs is decided by feed::livePeriodMs() and
// dashPeriodMs(): these are the rates while HTTP is carrying the glass.
//
//   GET /api/live        pollLiveMs (default 1000 ms)  rpm, speed, throttle,
//                                                      boost, coolantTemp,
//                                                      oilTemp, lockTarget,
//                                                      lockActual
//   GET /api/dashboard   pollDashMs (default 1000 ms)  mode, Haldex state,
//                                                      brake / lamp flags,
//                                                      CAN health
//
// Why two rates: /api/dashboard on the C6 calls getCPUUsagePercent(), which
// mallocs and walks every task on the single C6 core. Polling that at 10 Hz
// would cost roughly ten times the CPU for no extra visual resolution.
//
// Why /api/dashboard is polled at all, and never slower than ~2 s: it is the
// only handler that calls otaNoteWebActivity() on the C6, and the C6 shuts
// its access point down after ~5 minutes without it.
//
// A field that the C6 reports as JSON null is stored as NAN, so "sensor not
// present" and "sensor reads zero" stay distinguishable all the way to the UI.
// =====================================================================
#pragma once

#include <stdint.h>

namespace telemetry {

struct Live {
  float rpm, speed, throttle, boost;
  float coolant, oil;
  float lockTarget, lockActual;
  bool everOk;
  uint32_t ageMs;  // ms since the last successful parse
};

struct Dash {
  int mode;          // -1 when unknown
  int haldexState;   // -1 when unknown
  int lockTarget;
  int lockActual;
  bool chassisCAN;
  bool haldexCAN;
  bool brakeIn;
  bool handbrakeIn;
  bool tcOn;
  bool asrOn;
  bool awdWarning;
  bool couplingOpen;
  bool tempProtection;
  bool everOk;
  uint32_t ageMs;

  // Raw fields the C6 publishes that the dashboard does not use yet. They
  // live here rather than being parsed inside the UI so the raw page and the
  // lamp logic that follows it read the same value from the same place.
  //
  // brakeFromCAN / handbrakeFromCAN are the *chassis CAN* view of the pedals
  // (-1 = the C6 reported null). The wired inputs above them are a different
  // thing: on this car the wired handbrake input is off while the CAN says
  // the handbrake is up, which is the working theory for the dark lamps.
  float steeringAngle;    // deg, NAN when the C6 reports null (Gen1 gating)
  int brakeFromCAN;       // -1 unknown, else 0/1
  int handbrakeFromCAN;   // -1 unknown, else 0/1
};

struct Stats {
  uint32_t liveOk;
  uint32_t liveFail;
  uint32_t dashOk;
  uint32_t dashFail;
  uint32_t reconnects;
  int16_t rssi;
  bool wifiUp;
  uint32_t ip;
};

/// Start the network task (core 0). Called once from setup().
void begin();

/// Consistent snapshots.
Live live();
Dash dash();
Stats stats();

/// True when the last live sample is recent enough to be trusted.
bool liveFresh(uint32_t staleMs);

/// The least the network task's stack has ever had free, in bytes. 0 before it starts.
uint32_t netStackSpareBytes();

/// Force a reconnect + interval reset (called after settings change).
void restart();

/// Stop/start the HTTP polling without dropping the association, so the cost
/// of our own requests can be told apart from the radio receiving everything
/// else on the channel.
void setPaused(bool paused);

}  // namespace telemetry
