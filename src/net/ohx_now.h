#pragma once
// =====================================================================
// ESP-NOW listener for the OpenHaldex-C6's OHX_STATUS broadcast.
//
// Phase 1 is LISTEN ONLY. This file receives and decodes; it does not feed
// the dashboard yet. That keeps the HTTP link as the single source of truth
// while the wire format, the scaling of each field and the timing are
// checked against real data, and it means a mistake here cannot make the
// screen wrong - only the console.
//
// The contract itself (magic, types, sentinels, flags, struct layout) lives
// in include/ohx_proto.h, a verbatim copy of the C6's shared header. See
// docs/esp-now-plan.md.
// =====================================================================
#include <stdint.h>
#include <math.h>

namespace ohxnow {

/// Live counters, sampled by stats(). Cheap to read once a second.
struct Stats {
  uint32_t packets;     // OHX_STATUS packets accepted
  uint32_t rejects;     // dropped: bad magic / type / proto / length
  uint32_t duplicates;  // same seq as the previous packet from that sender
  uint32_t gaps;        // seq jumped by more than one: something was missed
  uint32_t senders;     // distinct source MACs heard
  float hz;             // accepted packets per second over the last second
  uint32_t ageMs;       // since the last accepted packet (UINT32_MAX = never)
  int16_t rssi;         // of the last accepted packet
  uint8_t channel;      // channel the last packet arrived on
  uint8_t src[6];       // last accepted sender
  bool enabled;         // ESP-NOW is initialised and the peer is registered
};

/// One decoded OHX_STATUS packet. A field the C6 marked as "no value"
/// (OHX_NONE8 / OHX_NONE16 / OHX_NONE_T) becomes NaN, exactly like a JSON
/// null on the HTTP side, so "absent" never has to be confused with zero.
struct Status {
  bool valid = false;
  uint8_t src[6] = {};
  uint8_t seq = 0, channel = 0;
  uint32_t nonce = 0;
  uint8_t fw[3] = {}, generation = 0, mode = 0, haldexState = 0;

  // Fast values - what /api/live carries today.
  float rpm = NAN, speed = NAN, throttle = NAN, boost = NAN;
  float lockTarget = NAN, lockActual = NAN;
  // Free extras: these have no equivalent in the HTTP API at all.
  float clutchTemp = NAN, oilTemp = NAN, moduleTemp = NAN;
  float supplyV = NAN, steering = NAN, slip[4] = {NAN, NAN, NAN, NAN};

  // flags[0]
  bool standalone = false, chassisCan = false, haldexCan = false, ctrlOff = false;
  bool busFail = false, modeOverride = false, canBcast = false, learned = false;
  // flags[1]
  bool tcForce = false, hazard = false, extBtn = false, brake = false;
  bool handbrake = false, tempProtection = false, couplingOpen = false, speedLimit = false;
  // flags[2]
  bool learn = false, longLearn = false, ctrlAccept = false, keyed = false;
  bool fixHunting = false, steerScale = false, analyzer = false, liveDiag = false;
  // flags[3]
  bool asrOff = false;

  char name[25] = {};
};

/// Initialise ESP-NOW. Returns false if WiFi is not up yet - poll() retries.
bool begin();

/// Turn the listener on or off at runtime, so its cost can be measured on
/// one boot (esp_now_deinit / esp_now_init under the hood).
bool setEnabled(bool on);

/// Call from the main loop: retries begin() until it works, refreshes the
/// rate/age counters, and prints the one-off decode of the first packet.
void poll();

bool enabled();

Stats stats();

/// Last accepted packet (valid = false until one arrives).
Status last();

/// Dump the last accepted packet: header, flags that are set, every value.
void printLast();

/// Side-by-side dump of ESP-NOW against the HTTP snapshot the UI is using,
/// which is how each field's scaling gets pinned down before it is trusted.
void printCompare();

}  // namespace ohxnow
