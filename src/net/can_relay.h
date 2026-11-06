#pragma once
// =====================================================================
// can_relay - the car's CAN frames, relayed by the controller over ESP-NOW.
//
// With "Relay CAN Frames to Gauges" switched on in its web UI, the controller
// broadcasts every frame it receives on its chassis bus (bus 0), batched into
// packets, in the wire format of include/cannow_proto.h. This keeps the latest
// copy of each id, which is what the CAN DATA page shows, and reads the few values
// the dashboard wants straight from it, so they stop needing an HTTP request.
//
// Not in the relay: bus 1, so the Haldex's own frames (its lock and status arrive
// in the OHX_STATUS packet instead), and anything the controller transmits itself.
//
// Broadcast has no retransmission, so this is a live view, not a lossless log. The
// counters say how much went missing, in the air and at the sender.
// =====================================================================
#include <math.h>
#include <stdint.h>

namespace canrelay {

constexpr int kMaxFrames = 200;  // distinct ids kept; PQ and MQB both use well under this

/// The latest copy of one id.
struct Frame {
  uint32_t id;
  bool extended;
  uint8_t length;
  uint8_t data[8];
  uint32_t count;   // copies received
  uint16_t perSec;  // received per second, over the last second
  uint32_t ageMs;   // since the last copy
};

struct Stats {
  uint32_t packets;   // relay packets accepted
  uint32_t frames;    // frames in them
  uint32_t airLost;   // packets missed in the air (gaps in the sender's sequence)
  uint32_t srcLost;   // frames the sender says it could not relay
  uint32_t rejects;   // malformed packets
  uint32_t tableFull; // frames dropped because kMaxFrames ids are already known
  uint32_t perSec;    // frames per second, over the last second
  int ids;            // distinct ids heard
  uint32_t ageMs;     // since the last packet (UINT32_MAX = never)
};

/// The values the dashboard reads from the relay. NAN / -1 = not heard recently,
/// or not known for this car.
struct Values {
  float coolant = NAN;  // C
  float oil = NAN;      // C, engine oil
  int brake = -1;       // 0 / 1
  int handbrake = -1;   // 0 / 1
};

/// Makes the frame tables in PSRAM. Call in setup(), before the network starts.
void begin();

/// One relay packet, from the ESP-NOW receive callback (the WiFi task). The caller
/// has already checked it came from our controller. Short, and never blocks.
void onPacket(const uint8_t *data, int length);

/// Call from the main loop: works out the per-second rates.
void poll();

/// True while relay packets are arriving.
bool fresh();

Stats stats();

/// How many distinct ids have been heard, and the frame in each slot, in the order
/// they were first heard. False if there is nothing in that slot.
int count();
bool frameAt(int index, Frame &out);

/// The latest copy of one id. False if it has never been heard.
bool find(uint32_t id, bool extended, Frame &out);

/// Read the dashboard's values for this Haldex generation's car.
Values values(uint8_t generation);

}  // namespace canrelay
