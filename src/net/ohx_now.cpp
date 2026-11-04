// =====================================================================
// ohx_now.cpp - ESP-NOW listener for the C6's OHX_STATUS broadcast.
//
// Nothing here sends. It validates the shared wire format, decodes it and
// counts what arrives, so the HTTP dashboard can be compared against it
// packet by packet before anything is switched over.
// =====================================================================
#include "net/ohx_now.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <string.h>

#include "net/telemetry.h"
#include "ohx_proto.h"

namespace ohxnow {
namespace {

portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;

// Written by the ESP-NOW receive callback (WiFi task), read by the main
// loop, hence the spinlock around every access to the data below.
Status g_status;
Stats g_stats;                      // packets / rejects / duplicates / senders
bool g_enabled = false;
bool g_peerAdded = false;
uint32_t g_readyAtMs = 0;

uint32_t g_lastPacketMs = 0;        // millis() of the last accepted packet
uint32_t g_lastSeqFrom[4] = {0};    // per-sender: seq of the previous packet
uint8_t g_senderMac[4][6] = {};
int g_senderCount = 0;

// Set by the callback, printed from poll() - never print from the callback,
// it runs in the WiFi task and a blocking UART write there is a bad idea.
volatile bool g_dumpRequest = false;
bool g_firstSeen = false;           // guarded by g_mux, never cleared
uint8_t g_dumpBuf[sizeof(OhxStatus)];
int g_dumpLen = 0;
int16_t g_dumpRssi = 0;
uint8_t g_dumpChannel = 0;

// ---- sentinel-aware decoders ------------------------------------------
inline float f8(uint8_t v) { return v == OHX_NONE8 ? NAN : (float)v; }
inline float f16(uint16_t v) { return v == OHX_NONE16 ? NAN : (float)v; }
inline float ftemp(int16_t v) { return v == OHX_NONE_T ? NAN : (float)v / 10.0f; }

// Which sender slot this MAC belongs to, adding it if it is new.
int senderSlot(const uint8_t *mac) {
  for (int i = 0; i < g_senderCount; i++) {
    if (memcmp(g_senderMac[i], mac, 6) == 0) return i;
  }
  if (g_senderCount < 4) {
    memcpy(g_senderMac[g_senderCount], mac, 6);
    return g_senderCount++;
  }
  return 0;  // more than four senders: fold into the first slot
}

void decode(const uint8_t *p, size_t len, const uint8_t *src, int16_t rssi, uint8_t channel,
            Status &out) {
  memcpy(out.src, src, 6);
  out.channel = channel;

  OhxStatus st;
  memcpy(&st, p, sizeof(st));

  out.seq = st.seq;
  out.nonce = st.nonce;
  memcpy(out.fw, st.fw, 3);
  out.generation = st.generation;
  out.mode = st.mode;
  out.haldexState = st.haldexState;

  out.rpm = f16(st.rpm);
  out.speed = f16(st.speed);
  out.throttle = f8(st.throttle);
  out.boost = f16(st.boost);          // mbar above ambient, clamped at 0
  out.lockTarget = f8(st.lockTarget);
  out.lockActual = f8(st.lockActual);
  out.clutchTemp = ftemp(st.clutchTemp);
  out.oilTemp = ftemp(st.oilTemp);
  out.moduleTemp = ftemp(st.moduleTemp);
  out.supplyV = st.supplyMv == OHX_NONE16 ? NAN : (float)st.supplyMv / 1000.0f;
  out.steering = f16(st.steering);
  for (int i = 0; i < 4; i++) out.slip[i] = st.slip[i] == -128 ? NAN : (float)st.slip[i];

  const uint8_t *f = st.flags;
  out.standalone = f[0] & OHX_F0_STANDALONE;
  out.chassisCan = f[0] & OHX_F0_CHASSIS_CAN;
  out.haldexCan = f[0] & OHX_F0_HALDEX_CAN;
  out.ctrlOff = f[0] & OHX_F0_CTRL_OFF;
  out.busFail = f[0] & OHX_F0_BUS_FAIL;
  out.modeOverride = f[0] & OHX_F0_OVERRIDE;
  out.canBcast = f[0] & OHX_F0_CAN_BCAST;
  out.learned = f[0] & OHX_F0_LEARNED;

  out.tcForce = f[1] & OHX_F1_TC_FORCE;
  out.hazard = f[1] & OHX_F1_HAZARD;
  out.extBtn = f[1] & OHX_F1_EXT_BTN;
  out.brake = f[1] & OHX_F1_BRAKE;
  out.handbrake = f[1] & OHX_F1_HANDBRAKE;
  out.tempProtection = f[1] & OHX_F1_TEMP_PROT;
  out.couplingOpen = f[1] & OHX_F1_COUPLING_OPEN;
  out.speedLimit = f[1] & OHX_F1_SPEED_LIMIT;

  out.learn = f[2] & OHX_F2_LEARN;
  out.longLearn = f[2] & OHX_F2_LONG_LEARN;
  out.ctrlAccept = f[2] & OHX_F2_CONTROL;
  out.keyed = f[2] & OHX_F2_KEY;
  out.fixHunting = f[2] & OHX_F2_FIX_HUNTING;
  out.steerScale = f[2] & OHX_F2_STEER_SCALE;
  out.analyzer = f[2] & OHX_F2_ANALYZER;
  out.liveDiag = f[2] & OHX_F2_LIVE_DIAG;

  out.asrOff = f[3] & OHX_F3_ASR_OFF;

  memcpy(out.name, st.name, sizeof(st.name));
  out.name[24] = '\0';
  out.valid = true;
  (void)len;
}

// ---- receive callback --------------------------------------------------
// Runs in the WiFi task. Validate, decode, count; no printing, no loops
// that can block, and take the spinlock only around the shared state.
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != (int)sizeof(OhxStatus) || data[0] != OHX_MAGIC0 || data[1] != OHX_MAGIC1 ||
      data[2] != OHX_T_STATUS || data[3] != OHX_PROTO) {
    taskENTER_CRITICAL(&g_mux);
    g_stats.rejects++;
    taskEXIT_CRITICAL(&g_mux);
    return;
  }

  const uint32_t now = millis();
  // rssi is signed: an int8_t in the rx control block, so read it as one.
  const int8_t rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
  const uint8_t ch = info->rx_ctrl ? (uint8_t)info->rx_ctrl->channel : 0;

  Status s;
  decode(data, (size_t)len, info->src_addr, rssi, ch, s);

  taskENTER_CRITICAL(&g_mux);
  const int slot = senderSlot(info->src_addr);
  // seq is a uint8 counter with wraparound, so compare with (uint8_t)(a-b).
  const uint32_t prev = g_lastSeqFrom[slot];
  if (g_stats.packets > 0 && (uint8_t)(s.seq - (uint8_t)prev) == 0) {
    g_stats.duplicates++;
  } else if (g_stats.packets > 0 && (uint8_t)(s.seq - (uint8_t)prev) > 1) {
    g_stats.gaps++;
  }
  g_lastSeqFrom[slot] = s.seq;

  g_status = s;
  g_stats.packets++;
  g_stats.senders = (uint32_t)g_senderCount;
  g_stats.rssi = (int16_t)rssi;
  g_stats.channel = ch;
  memcpy(g_stats.src, info->src_addr, 6);
  g_lastPacketMs = now;

  // Exactly the first accepted packet is decoded onto the console. poll()
  // runs far more often than packets arrive, so the "already dumped" state
  // has to live here, not in the flag poll() clears.
  if (!g_firstSeen) {
    g_firstSeen = true;
    memcpy(g_dumpBuf, data, sizeof(OhxStatus));
    g_dumpLen = len;
    g_dumpRssi = (int16_t)rssi;
    g_dumpChannel = ch;
    g_dumpRequest = true;
  }
  taskEXIT_CRITICAL(&g_mux);
}

void hexdump(const uint8_t *p, int len) {
  for (int i = 0; i < len; i += 16) {
    Serial.printf("  %04x ", i);
    for (int j = 0; j < 16 && i + j < len; j++) Serial.printf("%02x ", p[i + j]);
    for (int j = (len - i < 16 ? len - i : 16); j < 16; j++) Serial.print("   ");
    Serial.print(" |");
    for (int j = 0; j < 16 && i + j < len; j++) {
      const uint8_t c = p[i + j];
      Serial.print((c >= 32 && c < 127) ? (char)c : '.');
    }
    Serial.println("|");
  }
}

void printFlags(const Status &s) {
  struct Bit {
    bool on;
    const char *name;
  };
  const Bit bits[] = {
      {s.standalone, "STANDALONE"}, {s.chassisCan, "CHASSIS_CAN"}, {s.haldexCan, "HALDEX_CAN"},
      {s.ctrlOff, "CTRL_OFF"},      {s.busFail, "BUS_FAIL"},       {s.modeOverride, "OVERRIDE"},
      {s.canBcast, "CAN_BCAST"},    {s.learned, "LEARNED"},        {s.tcForce, "TC_FORCE"},
      {s.hazard, "HAZARD"},         {s.extBtn, "EXT_BTN"},         {s.brake, "BRAKE"},
      {s.handbrake, "HANDBRAKE"},   {s.tempProtection, "TEMP_PROT"},
      {s.couplingOpen, "COUPLING_OPEN"}, {s.speedLimit, "SPEED_LIMIT"}, {s.learn, "LEARN"},
      {s.longLearn, "LONG_LEARN"},  {s.ctrlAccept, "CTRL_ACCEPT"}, {s.keyed, "KEYED"},
      {s.fixHunting, "FIX_HUNTING"}, {s.steerScale, "STEER_SCALE"},
      {s.analyzer, "ANALYZER"},     {s.liveDiag, "LIVE_DIAG"},     {s.asrOff, "ASR_OFF"},
  };
  Serial.print("  flags   :");
  int n = 0;
  for (const Bit &b : bits) {
    if (!b.on) continue;
    Serial.printf(" %s", b.name);
    n++;
  }
  if (!n) Serial.print(" none");
  Serial.println();
}

const char *modeName(uint8_t m) {
  static const char *kNames[] = {"stock", "FWD", "50:50", "60:40", "75:25", "expert"};
  return m < 6 ? kNames[m] : "?";
}

void printValue(const char *name, float v, const char *unit) {
  if (isnan(v)) {
    Serial.printf("  %-10s  -- %s\n", name, unit);
  } else {
    Serial.printf("  %-10s  %7.2f %s", name, (double)v, unit);
    Serial.println();
  }
}

/// Short "value or --" string from a rotating set of buffers, so several can
/// be used inside one printf.
const char *fstr(float v) {
  static char bufs[4][16];
  static int i = 0;
  char *b = bufs[i = (i + 1) & 3];
  if (isnan(v)) {
    snprintf(b, 16, "--");
  } else {
    snprintf(b, 16, "%.0f", (double)v);
  }
  return b;
}

}  // namespace

bool begin() {
  if (g_enabled) return true;
  if (WiFi.getMode() == WIFI_MODE_NULL) return false;  // WiFi must be started first

  const esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("[now] esp_now_init failed: %d\n", (int)err);
    g_readyAtMs = millis();
    return false;
  }
  esp_now_register_recv_cb(onRecv);

  // A peer entry is only needed to *send*. Registering the broadcast address
  // now means Phase 3 (commands) has the plumbing in place, and it gives a
  // definite answer to "does the peer table accept a peer on this interface".
  esp_now_peer_info_t peer = {};
  memset(peer.peer_addr, 0xFF, 6);
  peer.channel = 0;  // 0 = whatever channel the station is on: we follow the C6
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  const esp_err_t perr = esp_now_add_peer(&peer);
  g_peerAdded = (perr == ESP_OK);

  g_enabled = true;
  taskENTER_CRITICAL(&g_mux);
  g_firstSeen = false;  // re-arming gives a fresh decode on the console
  taskEXIT_CRITICAL(&g_mux);
  Serial.printf("[now] listening: peer %s (%d), own mac %s, station channel %u\n",
                g_peerAdded ? "added" : "ADD FAILED", (int)perr,
                WiFi.macAddress().c_str(), (unsigned)WiFi.channel());
  return true;
}

void poll() {
  if (!g_enabled) {
    // WiFi comes up asynchronously in the network task, so retry politely
    // (once a second) rather than failing at boot.
    if ((uint32_t)(millis() - g_readyAtMs) < 1000) return;
    g_readyAtMs = millis();
    if (!begin()) return;
  }

  if (g_dumpRequest) {
    taskENTER_CRITICAL(&g_mux);
    g_dumpRequest = false;
    const int len = g_dumpLen;
    uint8_t buf[sizeof(OhxStatus)];
    memcpy(buf, g_dumpBuf, sizeof(buf));
    const int16_t rssi = g_dumpRssi;
    const uint8_t ch = g_dumpChannel;
    const Status s = g_status;
    taskEXIT_CRITICAL(&g_mux);

    Serial.printf("\n[now] first OHX_STATUS: %d bytes from %02X:%02X:%02X:%02X:%02X:%02X"
                  " rssi %d dBm, channel %u (station is on %u)\n",
                  len, s.src[0], s.src[1], s.src[2], s.src[3], s.src[4], s.src[5], (int)rssi,
                  (unsigned)ch, (unsigned)WiFi.channel());
    Serial.printf("  header  : magic %c%c type %u proto %u, fw %u.%u.%u, generation %u\n", buf[0],
                  buf[1], buf[2], buf[3], s.fw[0], s.fw[1], s.fw[2], (unsigned)s.generation);
    Serial.printf("  seq %u  nonce %08lx  name '%s'  struct %u bytes (ours %u)\n",
                  (unsigned)s.seq, (unsigned long)s.nonce, s.name, (unsigned)sizeof(OhxStatus),
                  (unsigned)sizeof(OhxStatus));
    printFlags(s);
    printLast();
    Serial.println("[now] raw bytes:");
    hexdump(buf, len);
    Serial.println();
  }

  // Packet rate over a rolling ~1 s window: the C6 broadcasts every 100 ms,
  // so this should read ~10.0 Hz. Anything less means airtime is being lost.
  static uint32_t lastMs = 0;
  static uint32_t lastPackets = 0;
  const uint32_t now = millis();
  if ((uint32_t)(now - lastMs) >= 1000) {
    const Stats st = stats();
    const uint32_t dt = now - lastMs;
    const float hz = (float)(st.packets - lastPackets) * 1000.0f / (float)dt;
    lastMs = now;
    lastPackets = st.packets;
    taskENTER_CRITICAL(&g_mux);
    g_stats.hz = hz;
    taskEXIT_CRITICAL(&g_mux);
  }
}

bool enabled() { return g_enabled; }

bool setEnabled(const bool on) {
  if (on == g_enabled) return true;
  if (on) return begin();

  // Stop listening: unregister first so no callback can run once the
  // driver state is being torn down.
  esp_now_unregister_recv_cb();
  if (g_peerAdded) {
    uint8_t bcast[6];
    memset(bcast, 0xFF, 6);
    esp_now_del_peer(bcast);
    g_peerAdded = false;
  }
  esp_now_deinit();
  g_enabled = false;
  Serial.println("[now] listener off");
  return true;
}

Stats stats() {
  Stats out;
  taskENTER_CRITICAL(&g_mux);
  out = g_stats;
  const uint32_t last = g_lastPacketMs;
  taskEXIT_CRITICAL(&g_mux);
  out.enabled = g_enabled;
  out.ageMs = last ? (uint32_t)(millis() - last) : UINT32_MAX;
  return out;
}

Status last() {
  Status out;
  taskENTER_CRITICAL(&g_mux);
  out = g_status;
  taskEXIT_CRITICAL(&g_mux);
  return out;
}

void printLast() {
  const Status s = last();
  if (!s.valid) {
    Serial.println("  (no packet yet)");
    return;
  }
  Serial.printf("  mode    : %u (%s), haldexState %u\n", (unsigned)s.mode, modeName(s.mode),
                (unsigned)s.haldexState);
  printValue("lockTarget", s.lockTarget, "%");
  printValue("lockActual", s.lockActual, "%");
  printValue("rpm", s.rpm, "");
  printValue("speed", s.speed, "km/h");
  printValue("throttle", s.throttle, "%");
  printValue("boost", s.boost, "mbar above ambient");
  printValue("clutchTemp", s.clutchTemp, "C");
  printValue("oilTemp", s.oilTemp, "C");
  printValue("moduleTemp", s.moduleTemp, "C");
  printValue("supplyV", s.supplyV, "V");
  printValue("steering", s.steering, "deg");
  Serial.printf("  slip FL/FR/RL/RR: %s %s %s %s %%\n", fstr(s.slip[0]), fstr(s.slip[1]),
                fstr(s.slip[2]), fstr(s.slip[3]));
}

namespace {
void row(const char *name, float now_, float http, const char *unit) {
  char a[24], b[24];
  if (isnan(now_)) snprintf(a, sizeof(a), "--"); else snprintf(a, sizeof(a), "%.2f", (double)now_);
  if (isnan(http)) snprintf(b, sizeof(b), "--"); else snprintf(b, sizeof(b), "%.2f", (double)http);
  Serial.printf("  %-11s %10s   %10s %s\n", name, a, b, unit);
}
}  // namespace

void printCompare() {
  const Status s = last();
  const telemetry::Live l = telemetry::live();
  const telemetry::Dash d = telemetry::dash();
  const Stats st = stats();

  Serial.printf("\n[now] ESP-NOW vs HTTP   (packets %lu, rej %lu, dup %lu, gap %lu, %.1f Hz, "
                "age %lu ms, %lu sender(s), rssi %d)\n",
                (unsigned long)st.packets, (unsigned long)st.rejects, (unsigned long)st.duplicates,
                (unsigned long)st.gaps, (double)st.hz,
                st.ageMs == UINT32_MAX ? 0UL : (unsigned long)st.ageMs,
                (unsigned long)st.senders, (int)st.rssi);
  if (!s.valid) Serial.println("  (no ESP-NOW packet received yet)");
  Serial.printf("  %-11s %10s   %10s\n", "field", "ESP-NOW", "HTTP");
  row("rpm", s.rpm, l.rpm, "");
  row("speed", s.speed, l.speed, "km/h");
  row("throttle", s.throttle, l.throttle, "%");
  row("boost", s.boost, l.boost, "mbar");
  row("lockTarget", s.lockTarget, l.lockTarget, "%");
  row("lockActual", s.lockActual, l.lockActual, "%");
  row("coolant", NAN, l.coolant, "C  (HTTP only)");
  row("oil", s.oilTemp, l.oil, "C  (ESP-NOW: oilTemp)");
  row("clutch", s.clutchTemp, NAN, "C  (ESP-NOW only)");
  row("module", s.moduleTemp, NAN, "C  (ESP-NOW only)");
  row("supply", s.supplyV, NAN, "V  (ESP-NOW only)");
  Serial.printf("  %-11s %10u   %10d   mode\n", "mode", (unsigned)s.mode, (int)d.mode);
  Serial.printf("  %-11s %10u   %10d   haldexState\n", "haldexState", (unsigned)s.haldexState,
                (int)d.haldexState);
  Serial.printf("  %-11s %10s   %10s   brake\n", "brake", s.brake ? "yes" : "no",
                d.brakeIn ? "yes" : "no");
  Serial.printf("  %-11s %10s   %10s   handbrake\n", "handbrake", s.handbrake ? "yes" : "no",
                d.handbrakeIn ? "yes" : "no");
  Serial.printf("  %-11s %10s   %10s   tempProtection\n", "tempProt", s.tempProtection ? "yes" : "no",
                d.tempProtection ? "yes" : "no");
  Serial.printf("  %-11s %10s   %10s   coupling open\n", "coupOpen", s.couplingOpen ? "yes" : "no",
                d.couplingOpen ? "yes" : "no");
  Serial.printf("  %-11s %10s   %10s   chassis CAN\n", "chassisCAN", s.chassisCan ? "yes" : "no",
                d.chassisCAN ? "yes" : "no");
  Serial.printf("  %-11s %10s   %10s   haldex CAN\n", "haldexCAN", s.haldexCan ? "yes" : "no",
                d.haldexCAN ? "yes" : "no");
  Serial.println();
}

}  // namespace ohxnow
