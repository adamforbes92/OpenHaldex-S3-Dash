#pragma once
// =====================================================================
// dbc - a compact, read-only CAN database in flash.
//
// The tables themselves are in dbc_tables.cpp, generated from dbc/*.dbc by
// tools/make_dbc_tables.py. They are what turns a relayed frame on the CAN DATA
// page into named signals: VW PQ (Mk4 / Mk5 era) and VW MQB.
//
// This is the viewer's data, not the dashboard's. The tiles and lamps read the
// few bytes they need directly (see can_relay.cpp), the same bytes the controller
// has proved on a car, because a DBC can be wrong: vw_pq lumps the handbrake bit
// in with the brake-fluid warning, for one.
// =====================================================================
#include <stdint.h>

namespace dbc {

constexpr uint8_t kMuxPlain = 0xFF;     // always present in the frame
constexpr uint8_t kMuxSelector = 0xFE;  // this signal says which others are present

struct Scale {
  float factor;
  float offset;
  uint8_t unit;  // index into Table::units
};

// One signal: 8 bytes in flash, plus its name.
struct Signal {
  uint16_t nameAt;   // offset into Table::names
  uint16_t scaleAt;  // index into Table::scales
  uint8_t start;     // start bit, as written in the DBC
  uint8_t length;    // in bits
  uint8_t bigEndian;
  uint8_t mux;       // kMuxPlain, kMuxSelector, or the selector value it needs
};

struct Message {
  uint32_t id;
  uint16_t nameAt;
  uint16_t firstSignal;  // index into Table::signals
  uint8_t signalCount;
  uint8_t dlc;
  uint8_t extended;
};

struct Table {
  const char *name;
  const Message *messages;  // sorted by id
  uint16_t messageCount;
  const Signal *signals;
  const Scale *scales;
  const char *const *units;
  const char *names;
};

extern const Table kVwPq;
extern const Table kVwMqb;

/// Which table a Haldex generation's car uses, or nullptr for the other makes
/// (GM, Ford, Volvo, SAAB), whose frames are then shown as raw bytes only.
const Table *forGeneration(uint8_t generation);

/// The message with this id, or nullptr if the DBC does not know it.
const Message *find(const Table &table, uint32_t id, bool extended);

/// Is this signal in this frame? A multiplexed signal only exists when the
/// message's selector holds its value.
bool isActive(const Table &table, const Message &message, const Signal &signal,
              const uint8_t *data, uint8_t length);

/// The signal's value in engineering units. False if the frame is too short to
/// hold it.
bool read(const Table &table, const Signal &signal, const uint8_t *data, uint8_t length,
          double &value);

const char *name(const Table &table, uint16_t nameAt);
const char *unit(const Table &table, const Signal &signal);

}  // namespace dbc
