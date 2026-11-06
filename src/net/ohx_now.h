#pragma once
// =====================================================================
// ESP-NOW link to the OpenHaldex-C6: the OHX_STATUS broadcast, and the
// commands that go the other way.
//
// In: the controller broadcasts a packed status packet every 100 ms. This
// decodes it, and datafeed.cpp prefers it over HTTP while packets are
// arriving.
//
// Out: exactly one thing is transmitted - a signed mode command (sendMode),
// raised by tapping the HALDEX panel. The controller answers in the ack
// fields of its next broadcast, never directly, so lastAck() reports the
// result of the last command this dash sent.
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
  uint32_t ignored;     // packets from a controller that is not ours
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

  // The controller's answer to the last command anyone sent it. It does not
  // reply over the air: it publishes the result in its next broadcast status,
  // which is why these arrive on the same 10 Hz stream as everything else.
  uint8_t ackSrc[2] = {}, ackSeq = 0, ackResult = 0;
};

/// Load the remembered controller from NVS. Call once from setup(), before the
/// network task starts, so its first join is already aimed at the right car.
void loadController();

/// The controller this dash follows: true and its MAC if one has been chosen.
/// Safe from any task.
bool controller(uint8_t mac[6]);

/// Follow this controller from now on. The network task uses it when it joins
/// an access point before any packet has been heard. Saved by poll().
void setController(const uint8_t mac[6]);

/// Stop following it, so the next controller heard with our SSID is taken
/// instead. How a replaced controller gets picked up: changing the SSID, the
/// DEFAULTS button, or 'f' on the console. Saved by poll().
void forgetController();

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

/// Result of the last command *this* dash sent, taken from the ack fields of
/// the controller's next status packet. valid = the controller has answered
/// something we sent; ageMs = how long ago.
struct Ack {
  bool valid;
  uint8_t result;  // ohxResult_t
  uint8_t seq;     // the seq we sent
  uint32_t ageMs;
};
Ack lastAck();

/// Ask the controller to change mode (ohxCmd_t OHX_C_MODE): 0 Stock, 1 FWD,
/// 2 50:50, 3 60:40, 4 75:25, 5 Expert. Returns false if ESP-NOW is not up,
/// or the controller has not been heard from yet (its nonce is part of the
/// signature, so there is nothing to sign against until it speaks).
///
/// Not thread-safe with begin()/setEnabled(): call it from the UI task, as
/// the dashboard does.
bool sendMode(uint8_t mode);

/// Dump the last accepted packet: header, flags that are set, every value.
void printLast();

/// Side-by-side dump of ESP-NOW against the HTTP snapshot the UI is using,
/// which is how each field's scaling gets pinned down before it is trusted.
void printCompare();

}  // namespace ohxnow
