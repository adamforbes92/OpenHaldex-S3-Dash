// =====================================================================
// capability.cpp - see capability.h for why any of this is a decision at all
// =====================================================================
#include "app/capability.h"

#include <Arduino.h>

#include <math.h>
#include <stdio.h>

#include "net/ohx_now.h"

namespace cap {
namespace {

Caps g;

// Latched detections: a field is "available" if it has *ever* carried a
// value, not if it carries one right now. A cold engine sends no oil
// temperature for a while, and that must not take a widget off the glass.
bool g_sawSupply = false;
bool g_sawTemps = false;
bool g_sawSteer = false;

uint32_t g_firstPacketMs = 0;
char g_summary[128] = "waiting for data";

// 20 packets is ~2 s at 10 Hz: long enough that a sentinel-only field has had
// a fair chance to prove otherwise, short enough that the layout settles
// before the driver has pulled away.
constexpr uint16_t kSettlePackets = 20;
// If the packets stop or never start, decide anyway rather than sit
// undecided: an unsettled dashboard would keep whatever shape it booted with.
constexpr uint32_t kSettleMs = 8000;

// Boost is the one field with no sentinel, so it comes from the generation
// table (see capability.h). Gen1 is taken to have no boost source, as agreed
// for this car; every other generation is assumed to have one. One line to
// change if a Gen1 owner reports boost working.
bool boostForGeneration(uint8_t gen) { return gen != 1; }

}  // namespace

void poll() {
  if (g.settled) return;

  const uint32_t now = millis();
#if OHX_USE_ESP_NOW
  const ohxnow::Status s = ohxnow::last();
  if (s.valid) {
    if (!g.genKnown) {
      g.genKnown = true;
      g.generation = s.generation;
      g_firstPacketMs = now;
    }
    // A sentinel reaches us as NaN: the same "no value" the JSON spells null.
    if (!isnan(s.supplyV)) g_sawSupply = true;
    if (!isnan(s.clutchTemp) || !isnan(s.oilTemp) || !isnan(s.moduleTemp)) g_sawTemps = true;
    if (!isnan(s.steering)) g_sawSteer = true;
    g.packetsSeen++;
  }
  const bool havePackets = g.packetsSeen > 0;
#else
  const bool havePackets = false;
#endif

  const bool enough = g.packetsSeen >= kSettlePackets ||
                      (havePackets && (uint32_t)(now - g_firstPacketMs) >= kSettleMs);
  const bool givenUp = !havePackets && now >= kSettleMs;
  if (!enough && !givenUp) return;

  // With no packet there is no generation, so nothing is ruled out: the full
  // band is used rather than hiding a widget on a guess.
  g.boost = g.genKnown ? boostForGeneration(g.generation) : true;
  g.supplyVoltage = g_sawSupply;
  g.haldexTemps = g_sawTemps;
  g.steering = g_sawSteer;
  g.variant = g.boost ? layout::Variant::Full : layout::Variant::NoBoost;
  g.settled = true;

  Serial.printf("[cap] %s\n", summary());
}

const Caps &get() { return g; }

const char *summary() {
  // No Arduino String here: this runs on the UI task and a heap allocation
  // in a diagnostic path is a way to make the diagnostic the problem.
  char genTxt[12];
  if (g.genKnown) {
    snprintf(genTxt, sizeof(genTxt), "%u", (unsigned)g.generation);
  } else {
    snprintf(genTxt, sizeof(genTxt), "unknown");
  }
  snprintf(g_summary, sizeof(g_summary),
           "gen %s | boost %s | supply %s | haldex temps %s | steering %s | %s | %u pkts", genTxt,
           g.boost ? "yes (table)" : "no (table)",
           g.supplyVoltage ? "yes (detected)" : "no (detected)",
           g.haldexTemps ? "yes (detected)" : "no (detected)",
           g.steering ? "yes (detected)" : "no (detected)",
           layout::bandFor(g.variant).name, (unsigned)g.packetsSeen);
  return g_summary;
}

}  // namespace cap
