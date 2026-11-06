// =====================================================================
// config.cpp - NVS-backed dashboard configuration
// =====================================================================
#include "app/config.h"

#include <Arduino.h>
#include <Preferences.h>

#include <cmath>
#include <cstring>

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif

namespace cfg {
namespace {

constexpr const char *kNamespace = "ohxdash";
constexpr const char *kKey = "cfg";
constexpr uint32_t kMagic = 0x4F485844;  // 'OHXD'
constexpr uint16_t kVersion = 4;  // 4: pollLiveMs 100 -> 1000 (ESP-NOW is the
                                  //    fast path now); 3: + netUseStatic;
                                  //    2: staleMs 500 -> 1500

struct Blob {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  Data data;
};

Data g_data;
portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t g_revision = 1;

template <typename T>
T clampv(T v, T lo, T hi) {
  if (!(v == v)) return lo;  // NaN
  return v < lo ? lo : (v > hi ? hi : v);
}

void copyStr(char *dst, size_t dstLen, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, dstLen - 1);
  dst[dstLen - 1] = '\0';
}

// The settings that decide which controller the station talks to. Only these
// are worth dropping the WiFi link for: netUseStatic is left out because the
// network task learns it itself, on a link that is already up.
bool connectionDiffers(const Data &a, const Data &b) {
  return strcmp(a.ssid, b.ssid) != 0 || strcmp(a.pass, b.pass) != 0 ||
         strcmp(a.host, b.host) != 0 || a.port != b.port;
}

// Clamp every field into its legal range, in place.
void validate(Data &d) {
  // --- credentials ---------------------------------------------------
  d.ssid[sizeof(d.ssid) - 1] = '\0';
  d.pass[sizeof(d.pass) - 1] = '\0';
  d.host[sizeof(d.host) - 1] = '\0';
  if (d.ssid[0] == '\0') copyStr(d.ssid, sizeof(d.ssid), "OpenHaldex-C6");
  if (d.host[0] == '\0') copyStr(d.host, sizeof(d.host), "192.168.1.1");
  if (d.port == 0) d.port = 80;

  // --- thresholds ----------------------------------------------------
  d.rpmMax = clampv(d.rpmMax, 1000.0f, 20000.0f);
  d.rpmAmber = clampv(d.rpmAmber, 0.0f, d.rpmMax);
  d.rpmRedline = clampv(d.rpmRedline, d.rpmAmber, d.rpmMax);
  d.rpmShift = clampv(d.rpmShift, 0.0f, d.rpmMax);
  d.coolWarn = clampv(d.coolWarn, -40.0f, 200.0f);
  d.coolCrit = clampv(d.coolCrit, d.coolWarn, 250.0f);
  d.oilWarn = clampv(d.oilWarn, -40.0f, 200.0f);
  d.oilCrit = clampv(d.oilCrit, d.oilWarn, 250.0f);
  d.boostMax = clampv(d.boostMax, 200.0f, 5000.0f);
  d.boostWarn = clampv(d.boostWarn, 0.0f, d.boostMax);
  d.boostCrit = clampv(d.boostCrit, d.boostWarn, d.boostMax);

  if (d.boostUnits > BOOST_PSI) d.boostUnits = BOOST_BAR;
  d.brightness = clampv(d.brightness, 1.0f, 100.0f);

  // --- polling -------------------------------------------------------
  d.pollLiveMs = (uint16_t)clampv<int>(d.pollLiveMs, 50, 1000);
  d.pollDashMs = (uint16_t)clampv<int>(d.pollDashMs, 250, 2000);
  d.staleMs = (uint16_t)clampv<int>(d.staleMs, 200, 5000);
  d.netUseStatic = d.netUseStatic ? 1 : 0;
}

// Make these the live settings, in RAM only. Returns true if anything changed.
// The revision only moves when the connection did: it is what tells the network
// task to drop the link and rejoin, and the DAY button or a new rpm redline must
// not cost the dashboard its WiFi.
bool storeInRam(const Data &d) {
  taskENTER_CRITICAL(&g_mux);
  const bool changed = (memcmp(&g_data, &d, sizeof(Data)) != 0);
  const bool reconnect = connectionDiffers(g_data, d);
  g_data = d;
  if (reconnect) g_revision++;
  taskEXIT_CRITICAL(&g_mux);
  return changed;
}

void saveToNvs(const Data &d) {
  Blob blob{};
  blob.magic = kMagic;
  blob.version = kVersion;
  blob.data = d;

  Preferences prefs;
  if (prefs.begin(kNamespace, false)) {
    prefs.putBytes(kKey, &blob, sizeof(blob));
    prefs.end();
  } else {
    Serial.println("[cfg] NVS open failed - settings are RAM-only this boot");
  }
}

}  // namespace

Data defaults() {
  Data d{};
  copyStr(d.ssid, sizeof(d.ssid), "OpenHaldex-C6");
  copyStr(d.pass, sizeof(d.pass), "");
  copyStr(d.host, sizeof(d.host), "192.168.1.1");
  d.port = 80;

  d.rpmMax = 8000;
  d.rpmAmber = 6000;
  d.rpmRedline = 6800;
  d.rpmShift = 7300;
  d.coolWarn = 100;
  d.coolCrit = 110;
  d.oilWarn = 120;
  d.oilCrit = 135;
  d.boostMax = 2500;
  d.boostWarn = 1800;
  d.boostCrit = 2200;

  d.tempF = false;
  d.boostUnits = BOOST_BAR;
  d.brightness = 100.0f;

  // /api/live is no longer the fast path: rpm, speed, throttle, boost, oil
  // and the lock values arrive over ESP-NOW at 10 Hz. It stays at 1 Hz for
  // coolant temperature, and it is the fallback for everything if the
  // packets stop.
  d.pollLiveMs = 1000;
  d.pollDashMs = 1000;
  // 1.5 s, not the 500 ms the web page uses: over WiFi a single request can
  // legitimately take a few hundred milliseconds, and at 500 ms that showed
  // up as the stale overlay flashing on a link that was working fine.
  d.staleMs = 1500;
  d.netUseStatic = 0;

  validate(d);
  return d;
}

void begin() {
  // Defaults first, in RAM only, so g_data is valid even if NVS is empty or
  // corrupt. They must not be saved here: that is what used to overwrite the
  // stored settings with the defaults on every boot, just before reading them.
  storeInRam(defaults());

  bool haveSaved = false;
  bool saveAgain = false;
  Preferences prefs;
  if (prefs.begin(kNamespace, true)) {
    Blob blob{};
    const size_t n = prefs.getBytesLength(kKey);
    // A version 3 blob has exactly the same field layout, so it can be read
    // and migrated rather than thrown away - throwing it away would also
    // throw away netUseStatic, which was learnt on the bench, and cost 15 s
    // of DHCP wait on the next boot.
    if (n == sizeof(blob) && prefs.getBytes(kKey, &blob, sizeof(blob)) == sizeof(blob) &&
        blob.magic == kMagic && (blob.version == kVersion || blob.version == 3)) {
      Data d = blob.data;
      if (blob.version < 4) {
        d.pollLiveMs = 1000;  // the fast values no longer come from HTTP
        Serial.println("[cfg] migrating settings: /api/live 100 ms -> 1000 ms (ESP-NOW)");
        saveAgain = true;
      }
      validate(d);
      if (memcmp(&d, &blob.data, sizeof(Data)) != 0) saveAgain = true;  // something was clamped
      storeInRam(d);
      haveSaved = true;
    }
    prefs.end();
  }

  if (haveSaved) {
    // Booting the dash must not burn an NVS page every time: only write back
    // what actually differs from what was loaded.
    if (saveAgain) saveToNvs(get());
    return;
  }

  // First boot, or a blob this version cannot read. The optional compile-time
  // credentials for bench use are only a seed: once settings exist they win,
  // as include/wifi_secrets.h.example says. wifi_secrets.h is gitignored; the
  // .example file shows the symbols.
#if defined(OHX_WIFI_SSID)
  Data d = get();
  copyStr(d.ssid, sizeof(d.ssid), OHX_WIFI_SSID);
#if defined(OHX_WIFI_PASS)
  copyStr(d.pass, sizeof(d.pass), OHX_WIFI_PASS);
#endif
#if defined(OHX_HOST)
  copyStr(d.host, sizeof(d.host), OHX_HOST);
#endif
  validate(d);
  storeInRam(d);
#endif
  saveToNvs(get());
}

void apply(const Data &in) {
  Data d = in;
  validate(d);
  // An unchanged value is not written: flash pages are not free.
  if (storeInRam(d)) saveToNvs(d);
}

void requestReconnect() {
  taskENTER_CRITICAL(&g_mux);
  g_revision++;
  taskEXIT_CRITICAL(&g_mux);
}

const Data &get() { return g_data; }

void connectionCopy(char *ssid, size_t ssidLen, char *pass, size_t passLen, char *host,
                    size_t hostLen, uint16_t &port) {
  taskENTER_CRITICAL(&g_mux);
  copyStr(ssid, ssidLen, g_data.ssid);
  copyStr(pass, passLen, g_data.pass);
  copyStr(host, hostLen, g_data.host);
  port = g_data.port;
  taskEXIT_CRITICAL(&g_mux);
}

uint32_t revision() { return g_revision; }

}  // namespace cfg
