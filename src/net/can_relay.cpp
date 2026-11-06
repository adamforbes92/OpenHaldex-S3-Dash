// =====================================================================
// can_relay.cpp - see can_relay.h.
// =====================================================================
#include "net/can_relay.h"

#include <Arduino.h>
#include <string.h>

#include "app/ram.h"
#include "cannow_proto.h"
#include "net/dbc.h"

// One entry for each possible 11-bit CAN id, in the table g_slotOf11Bit.
#define SLOT_LOOKUP_ENTRIES 2048

namespace canrelay {
namespace {

// Relay packets stop arriving when the setting is off or the bus goes quiet, and
// this is what the "is the relay carrying the car" decisions go by.
constexpr uint32_t kFreshMs = 1000;
// How long a value read from a frame is believed. The same 2 s the controller
// gives Kombi_2 before its own API reports null.
constexpr uint32_t kValueFreshMs = 2000;
constexpr int kMaxExtended = 16;  // 29-bit ids are rare on these cars

struct Slot {
  uint32_t id;
  uint32_t count;
  uint32_t countLastSecond;
  uint32_t lastMs;
  uint16_t perSec;
  uint8_t length;
  bool extended;
  uint8_t data[8];
};

// Written by the WiFi task, read by the main loop and the network task, so every
// access goes through this lock. It is held for microseconds at a time.
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

// Both tables below are made in PSRAM by begin(), which keeps 7.6 KB out of internal
// RAM on every car, relay or no relay. They are read and written like normal arrays.
// Touching PSRAM inside the lock is fine: this runs in tasks, never in an interrupt
// that has to keep going during a flash write.
Slot *g_slots = nullptr;  // kMaxFrames entries
int g_used = 0;
// Which slot holds an 11-bit id: 0 = none yet, otherwise slot + 1. Looked up for
// every frame, so it is a direct index, not a search. kMaxFrames must stay under 255.
uint8_t *g_slotOf11Bit = nullptr;  // SLOT_LOOKUP_ENTRIES entries
bool g_tablesReady = false;        // true once both tables exist
uint8_t g_extendedSlots[kMaxExtended];
int g_extendedCount = 0;

Stats g_stats = {};
uint32_t g_lastPacketMs = 0;
uint8_t g_lastSeq = 0;
bool g_haveSeq = false;

uint32_t g_lastRateMs = 0;
uint32_t g_framesLastSecond = 0;

// The slot for this id, or -1. Call with the lock held.
int slotOf(uint32_t id, bool extended) {
  if (!g_tablesReady) return -1;  // no memory for the tables: nothing was ever heard
  if (!extended) {
    return (int)g_slotOf11Bit[id & CANN_TAG_ID] - 1;
  }
  for (int i = 0; i < g_extendedCount; i++) {
    const int slot = g_extendedSlots[i];
    if (g_slots[slot].id == id) return slot;
  }
  return -1;
}

// The slot for this id, taking a new one if it is the first time. -1 if full. Call
// with the lock held.
int slotOrNew(uint32_t id, bool extended) {
  int slot = slotOf(id, extended);
  if (slot >= 0) return slot;
  if (g_used >= kMaxFrames) return -1;
  if (extended && g_extendedCount >= kMaxExtended) return -1;

  slot = g_used;
  g_used++;
  memset(&g_slots[slot], 0, sizeof(Slot));
  g_slots[slot].id = id;
  g_slots[slot].extended = extended;
  if (extended) {
    g_extendedSlots[g_extendedCount] = (uint8_t)slot;
    g_extendedCount++;
  } else {
    g_slotOf11Bit[id & CANN_TAG_ID] = (uint8_t)(slot + 1);
  }
  return slot;
}

// A copy of a slot, for handing outside the lock.
void copyOut(const Slot &slot, uint32_t now, Frame &out) {
  out.id = slot.id;
  out.extended = slot.extended;
  out.length = slot.length;
  memcpy(out.data, slot.data, 8);
  out.count = slot.count;
  out.perSec = slot.perSec;
  out.ageMs = now - slot.lastMs;
}

// A frame heard recently enough to read a value from, at least minLength long.
bool recent(uint32_t id, uint8_t minLength, Frame &out) {
  if (!find(id, false, out)) return false;
  return out.ageMs <= kValueFreshMs && out.length >= minLength;
}

}  // namespace

void begin() {
  Serial.println("[relay] Setting up the frame tables...");
  g_slots = (Slot *)ram::psramCalloc(kMaxFrames, sizeof(Slot));
  g_slotOf11Bit = (uint8_t *)ram::psramCalloc(SLOT_LOOKUP_ENTRIES, sizeof(uint8_t));
  if (g_slots == nullptr || g_slotOf11Bit == nullptr) {
    // The dash still works without these: the values come over HTTP instead, and
    // the CAN DATA page stays empty.
    Serial.println("[relay] NO MEMORY for the frame tables - relay packets will be ignored");
    return;
  }
  g_tablesReady = true;
  Serial.println("[relay] Set up the frame tables!");
}

void onPacket(const uint8_t *data, const int length) {
  if (!g_tablesReady) return;
  const uint32_t now = millis();

  CannHdr header;
  if (length < (int)sizeof(CannHdr)) return;
  memcpy(&header, data, sizeof(header));
  if (header.proto != CANN_PROTO) {
    taskENTER_CRITICAL(&g_mux);
    g_stats.rejects++;
    taskEXIT_CRITICAL(&g_mux);
    return;
  }

  taskENTER_CRITICAL(&g_mux);
  // A gap in the sequence is packets lost in the air. A big jump backwards is the
  // sender rebooting, which lost nothing.
  if (g_haveSeq) {
    const uint8_t gap = (uint8_t)(header.seq - g_lastSeq - 1);
    if (gap < 128) g_stats.airLost += gap;
  }
  g_lastSeq = header.seq;
  g_haveSeq = true;
  g_stats.packets++;
  g_stats.srcLost += header.lost;
  g_lastPacketMs = now;

  // The frames, back to back: a 2-byte tag (the 11-bit id and the data length),
  // a 4-byte id instead if the tag says 29-bit, then the data bytes.
  int offset = sizeof(CannHdr);
  bool malformed = false;
  for (uint8_t i = 0; i < header.count; i++) {
    if (offset + 2 > length) {
      malformed = true;
      break;
    }
    uint16_t tag;
    memcpy(&tag, data + offset, 2);
    offset += 2;

    const uint8_t dataLength = (tag >> CANN_TAG_LEN_SHIFT) & 0x0F;
    const bool extended = (tag & CANN_TAG_EXT) != 0;
    uint32_t id = tag & CANN_TAG_ID;
    if (extended) {
      if (offset + 4 > length) {
        malformed = true;
        break;
      }
      memcpy(&id, data + offset, 4);
      id &= 0x1FFFFFFF;
      offset += 4;
    }
    if (dataLength > 8 || offset + dataLength > length) {
      malformed = true;
      break;
    }

    const int slotIndex = slotOrNew(id, extended);
    if (slotIndex < 0) {
      g_stats.tableFull++;
    } else {
      Slot &slot = g_slots[slotIndex];
      memcpy(slot.data, data + offset, dataLength);
      slot.length = dataLength;
      slot.lastMs = now;
      slot.count++;
    }
    offset += dataLength;
    g_stats.frames++;
  }
  if (malformed) g_stats.rejects++;
  taskEXIT_CRITICAL(&g_mux);
}

void poll() {
  const uint32_t now = millis();
  const uint32_t elapsed = now - g_lastRateMs;
  if (elapsed < 1000) return;
  g_lastRateMs = now;

  taskENTER_CRITICAL(&g_mux);
  g_stats.perSec = (uint32_t)((uint64_t)(g_stats.frames - g_framesLastSecond) * 1000 / elapsed);
  g_framesLastSecond = g_stats.frames;
  for (int i = 0; i < g_used; i++) {
    Slot &slot = g_slots[i];
    slot.perSec = (uint16_t)((uint64_t)(slot.count - slot.countLastSecond) * 1000 / elapsed);
    slot.countLastSecond = slot.count;
  }
  taskEXIT_CRITICAL(&g_mux);
}

bool fresh() {
  taskENTER_CRITICAL(&g_mux);
  const uint32_t last = g_lastPacketMs;
  taskEXIT_CRITICAL(&g_mux);
  return last != 0 && (uint32_t)(millis() - last) <= kFreshMs;
}

Stats stats() {
  taskENTER_CRITICAL(&g_mux);
  const uint32_t now = millis();
  Stats out = g_stats;
  out.ids = g_used;
  out.ageMs = g_lastPacketMs ? (now - g_lastPacketMs) : UINT32_MAX;
  taskEXIT_CRITICAL(&g_mux);
  return out;
}

int count() {
  taskENTER_CRITICAL(&g_mux);
  const int used = g_used;
  taskEXIT_CRITICAL(&g_mux);
  return used;
}

bool frameAt(const int index, Frame &out) {
  bool found = false;
  taskENTER_CRITICAL(&g_mux);
  const uint32_t now = millis();
  if (index >= 0 && index < g_used) {
    copyOut(g_slots[index], now, out);
    found = true;
  }
  taskEXIT_CRITICAL(&g_mux);
  return found;
}

bool find(const uint32_t id, const bool extended, Frame &out) {
  bool found = false;
  taskENTER_CRITICAL(&g_mux);
  // Inside the lock: taken before it, a packet landing in between would make the
  // frame newer than "now", and its age would wrap round to about 49 days.
  const uint32_t now = millis();
  const int slot = slotOf(id, extended);
  if (slot >= 0) {
    copyOut(g_slots[slot], now, out);
    found = true;
  }
  taskEXIT_CRITICAL(&g_mux);
  return found;
}

Values values(const uint8_t generation) {
  Values out;
  const dbc::Table *table = dbc::forGeneration(generation);
  if (table == nullptr) return out;  // another make: its ids mean something else
  Frame frame;

  // The same bytes the controller reads (OpenHaldexC6_can.cpp), on purpose: these
  // have been on a car, and they are not all what the DBC says. vw_pq lumps the
  // handbrake in with the brake-fluid warning as one 2-bit "Bremsinfo".
  if (table == &dbc::kVwPq) {
    if (recent(0x420, 5, frame)) {  // Kombi_2
      out.oil = frame.data[3] - 60.0f;
      out.coolant = frame.data[4] * 0.75f - 48.0f;
    }
    if (recent(0x288, 3, frame)) out.brake = frame.data[2] & 0x01;             // Motor_2: MO2_BLS
    if (recent(0x320, 2, frame)) out.handbrake = (frame.data[1] >> 1) & 0x01;  // Kombi_1: KO1_Handbremse
  } else {
    // MQB. The controller does not read Motor_07, so its API has no temperatures
    // on these cars. 254 and 255 mean "initialising" and "fault". Not yet checked
    // on a car: see TODO.
    if (recent(0x640, 4, frame)) {  // Motor_07
      if (frame.data[2] < 0xFE) out.oil = frame.data[2] - 60.0f;
      if (frame.data[3] < 0xFE) out.coolant = frame.data[3] * 0.75f - 48.0f;
    }
    if (recent(0x106, 4, frame)) out.brake = (frame.data[3] >> 2) & 0x01;      // ESP_05: ESP_Fahrer_bremst
    if (recent(0x30B, 3, frame)) out.handbrake = (frame.data[2] >> 7) & 0x01;  // Kombi_01: KBI_Handbremse
  }
  return out;
}

}  // namespace canrelay
