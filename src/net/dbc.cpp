// =====================================================================
// dbc.cpp - look up and decode signals from the generated tables.
// =====================================================================
#include "net/dbc.h"

namespace dbc {
namespace {

// Pull a signal's raw bits out of the frame. False if they run past its end.
bool rawBits(const Signal &signal, const uint8_t *data, uint8_t length, uint64_t &out) {
  if (length > 8) length = 8;

  if (signal.bigEndian) {
    // Big-endian ("Motorola") signals walk down through a byte, then jump to the
    // top of the next one. The VW files only have a handful.
    uint64_t value = 0;
    uint16_t bit = signal.start;
    for (uint8_t i = 0; i < signal.length; i++) {
      const uint8_t byteIndex = bit / 8;
      const uint8_t bitIndex = bit % 8;
      if (byteIndex >= length) return false;
      value = (value << 1) | ((data[byteIndex] >> bitIndex) & 1);
      if (bitIndex == 0) {
        bit += 15;
      } else {
        bit -= 1;
      }
    }
    out = value;
    return true;
  }

  // Little-endian ("Intel"): the whole frame as one number, then shift and mask.
  if (signal.start + signal.length > length * 8) return false;
  uint64_t frame = 0;
  for (uint8_t i = 0; i < length; i++) {
    frame |= (uint64_t)data[i] << (8 * i);
  }
  uint64_t value = frame >> signal.start;
  if (signal.length < 64) {
    value &= ((uint64_t)1 << signal.length) - 1;
  }
  out = value;
  return true;
}

}  // namespace

const Table *forGeneration(uint8_t generation) {
  switch (generation) {
    case 1:   // Gen1 02D
    case 2:   // Gen2 1K0, PQ35
    case 4:   // Gen4 0BR, PQ35 / PQ46
    case 51:  // 0AY, a PQ unit with the PQ frame set
      return &kVwPq;
    case 50:  // Gen5 MQB
    case 52:  // Gen5 MQB
      return &kVwMqb;
    default:
      return nullptr;
  }
}

const Message *find(const Table &table, uint32_t id, bool extended) {
  int low = 0;
  int high = (int)table.messageCount - 1;
  while (low <= high) {
    const int middle = (low + high) / 2;
    const Message &message = table.messages[middle];
    if (message.id == id) {
      return ((message.extended != 0) == extended) ? &message : nullptr;
    }
    if (message.id < id) {
      low = middle + 1;
    } else {
      high = middle - 1;
    }
  }
  return nullptr;
}

bool isActive(const Table &table, const Message &message, const Signal &signal,
              const uint8_t *data, uint8_t length) {
  if (signal.mux == kMuxPlain || signal.mux == kMuxSelector) return true;
  for (uint16_t i = 0; i < message.signalCount; i++) {
    const Signal &candidate = table.signals[message.firstSignal + i];
    if (candidate.mux != kMuxSelector) continue;
    uint64_t selector = 0;
    if (!rawBits(candidate, data, length, selector)) return false;
    return selector == signal.mux;
  }
  return false;  // a multiplexed signal with no selector: nothing says it is there
}

bool read(const Table &table, const Signal &signal, const uint8_t *data, uint8_t length,
          double &value) {
  uint64_t raw = 0;
  if (!rawBits(signal, data, length, raw)) return false;
  const Scale &scale = table.scales[signal.scaleAt];
  value = (double)raw * scale.factor + scale.offset;
  return true;
}

const char *name(const Table &table, uint16_t nameAt) { return table.names + nameAt; }

const char *unit(const Table &table, const Signal &signal) {
  return table.units[table.scales[signal.scaleAt].unit];
}

}  // namespace dbc
