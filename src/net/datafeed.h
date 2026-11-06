#pragma once
// =====================================================================
// datafeed - the one place that decides where the screen's numbers come
// from.
//
// ESP-NOW is the primary source: the C6 broadcasts an OHX_STATUS packet
// every 100 ms with no web stack involved. HTTP is the fallback, and it also
// supplies the fields the packet does not carry at all (coolant temperature,
// and the TC / ASR / AWD lamps, which the packet only has as "off" flags).
//
// The merge is deliberately narrow and easy to audit:
//
//   * source = ESP-NOW while a packet has arrived within kHoldMs
//   * source = HTTP when the packets stop or have never arrived
//   * a numeric field the packet marks as "no data" (OHX_NONE*) falls back to
//     the HTTP value if HTTP has one, so a field that this Haldex generation
//     never fills in cannot make the dashboard worse than it was
//   * coolant and engine oil are always HTTP; the lamps above are always HTTP.
//     The packet's oilTemp is the Haldex's oil, which is shown on the raw page
//     and never on the OIL tile
//
// Nothing is mixed silently: sourceName() is printed in the stats line and on
// every change of source, and 'e' on the console still dumps both transports
// side by side.
// =====================================================================
#include <stdint.h>

#include "net/telemetry.h"

namespace feed {

enum class Source : uint8_t {
  None,    // nothing has arrived yet
  EspNow,  // the packet is current: the glass is showing ESP-NOW data
  Http,    // packets stopped (or never came): the glass is on the HTTP poll
};

/// How long after the last packet we keep trusting it. Shorter than the
/// stale threshold on purpose: this decides which *source* is used, not
/// whether the screen is stale.
constexpr uint32_t kHoldMs = 500;

/// Drive this from the main loop: pumps the ESP-NOW listener and logs every
/// change of source.
void poll();

/// Merged snapshot for the UI.
telemetry::Live live();
telemetry::Dash dash();

/// True while the screen has current data from either transport.
bool fresh(uint32_t staleMs);

/// Which transport the fast values are coming from, and its name.
Source source();
const char *sourceName();

/// Age of the newest accepted ESP-NOW packet (UINT32_MAX = never).
uint32_t nowAgeMs();

/// True while ESP-NOW is the source in use.
bool onNow();

}  // namespace feed
