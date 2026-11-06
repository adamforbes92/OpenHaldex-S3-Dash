#pragma once
// =====================================================================
// datafeed - the one place that decides where the screen's numbers come
// from.
//
// ESP-NOW is the primary source: the C6 broadcasts an OHX_STATUS packet
// every 100 ms with no web stack involved, and with its CAN relay switched on
// it broadcasts the car's frames too. HTTP is the fallback, and it also supplies
// what neither of those has (the AWD lamp, and without the relay, coolant and
// engine oil).
//
// The merge is deliberately narrow and easy to audit:
//
//   * source = ESP-NOW while a packet has arrived within kHoldMs
//   * source = HTTP when the packets stop or have never arrived
//   * a numeric field the packet marks as "no data" (OHX_NONE*) falls back to
//     the HTTP value if HTTP has one, so a field that this Haldex generation
//     never fills in cannot make the dashboard worse than it was
//   * coolant, engine oil and the CAN view of the brake and handbrake come from
//     the CAN relay while it is carrying them, and from HTTP otherwise. The
//     packet's oilTemp is the Haldex's oil, which is shown on the raw page and
//     never on the OIL tile
//   * the TC and ASR lamps come from the packet's flags, the same two the
//     controller's API inverts; the AWD lamp is HTTP only
//
// It also decides how often HTTP is worth asking at all: see livePeriodMs().
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

/// How often each HTTP poll is worth making right now, in ms. 0 = not at all.
/// Every request costs the panel frames and costs the controller's single core
/// time, so the network task only asks for what nothing else is delivering:
///
///   packets stopped       both at the configured rate: HTTP is carrying the glass
///   packets, no relay     /api/live every 5 s (temperatures move slowly),
///                         /api/dashboard at the configured rate (brake, handbrake)
///   packets and relay     /api/live not at all, /api/dashboard every 2 s (the AWD
///                         lamp comes from Haldex data, which the relay lacks)
uint32_t livePeriodMs();
uint32_t dashPeriodMs();

/// Age of the newest accepted ESP-NOW packet (UINT32_MAX = never).
uint32_t nowAgeMs();

/// True while ESP-NOW is the source in use.
bool onNow();

}  // namespace feed
