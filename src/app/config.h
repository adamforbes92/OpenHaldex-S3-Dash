// =====================================================================
// config.h - persisted dashboard configuration (NVS)
//
// Mirrors DEFAULTS in dash.html so the two UIs behave identically:
//   rpmMax 8000  rpmAmber 6000  rpmRedline 6800  rpmShift 7300
//   coolWarn 100 coolCrit 110  oilWarn 120       oilCrit 135
//   boostMax 2500 boostWarn 1800 boostCrit 2200 (millibar gauge)
//   tempUnits "C"  boostUnits "bar"  brightness 1.0
//   pollLiveMs 100  pollDashMs 1000  staleMs 500
// =====================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace cfg {

enum BoostUnits : uint8_t { BOOST_BAR = 0, BOOST_MBAR = 1, BOOST_PSI = 2 };

struct Data {
  // Connection to the OpenHaldex-C6. Stored in NVS, never in git.
  char ssid[33];
  char pass[65];
  char host[48];
  uint16_t port;

  // Tachometer / warning thresholds.
  float rpmMax;
  float rpmAmber;
  float rpmRedline;
  float rpmShift;
  float coolWarn;
  float coolCrit;
  float oilWarn;
  float oilCrit;
  float boostMax;
  float boostWarn;
  float boostCrit;

  bool tempF;           // false = degC, true = degF
  uint8_t boostUnits;   // BoostUnits
  float brightness;     // 0..100 %

  // Polling. pollDashMs must stay <= 2000 to keep the C6's WiFi awake:
  // only /api/dashboard calls otaNoteWebActivity() on the C6, and it drops
  // the access point after ~5 minutes of inactivity.
  uint16_t pollLiveMs;
  uint16_t pollDashMs;
  uint16_t staleMs;

  // 1 = skip DHCP and go straight to the static address. Learnt at runtime:
  // set after the first successful static join, because the C6's AP leases no
  // address while it also sits on a home LAN with the same 192.168.1.0/24.
  // Waiting 15 s for a lease that never comes costs 15 s of a blank dashboard
  // on every boot and on every reconnect.
  uint8_t netUseStatic;
};

/// Load from NVS (or defaults + any build-time secrets on first boot).
void begin();

/// Validated, clamped, saved to NVS.
void apply(const Data &d);

/// The shipped defaults, validated. Nothing is applied or saved: the caller
/// decides what to keep (the DEFAULTS button keeps the connection) and hands
/// the result to apply().
Data defaults();

/// Current values. The UI thread owns writes; scalars are read directly by
/// the network task, strings are copied under a lock.
const Data &get();

/// Copy of the connection fields only, safe against a concurrent apply().
void connectionCopy(char *ssid, size_t ssidLen, char *pass, size_t passLen, char *host,
                    size_t hostLen, uint16_t &port);

/// Bumped when the connection settings change (ssid, password, host, port), so
/// the network task knows to rejoin. Nothing else moves it: rejoining means a
/// few seconds of stale data, which a brightness change must not cost.
uint32_t revision();

/// Ask the network task to rejoin even though nothing changed.
void requestReconnect();

}  // namespace cfg
