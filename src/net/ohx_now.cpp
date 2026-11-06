// =====================================================================
// ohx_now.cpp - the ESP-NOW link to the C6.
//
// Receives and decodes the OHX_STATUS broadcast, and sends the one command
// class this dash raises: OHX_C_MODE, from a tap on the HALDEX panel. The
// send path is small and deliberate - build the 22-byte OhxCommand, sign
// everything before the hmac field with HMAC-SHA256 keyed on the controller's
// access-point password, unicast it to the address the last status came from,
// and read the answer out of the ack fields of the next broadcast.
// =====================================================================
#include "net/ohx_now.h"

#include <mbedtls/md.h>

#include <cstring>

#include "app/config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <stddef.h>
#include <string.h>

#include "net/telemetry.h"
#include "net/can_relay.h"
#include "net/datafeed.h"
#include "cannow_proto.h"
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

// ---- the command path. Everything here is written by the UI task and read
// by the receive callback (or the other way round), so only plain scalars and
// the module's existing critical section are used - no allocation, no locks
// held across a send.
uint8_t g_ownMac[6] = {};           // so an ack can be recognised as ours
uint8_t g_cmdSeq = 0;               // our own counter, matched against ackSeq
uint32_t g_lastCmdMs = 0;
volatile bool g_ackValid = false;
volatile uint8_t g_ackResult = 0, g_ackSeq = 0;
volatile uint32_t g_ackMs = 0;
uint32_t g_lastStatusMs = 0;        // when the nonce we would sign against arrived
uint32_t g_lastNonce = 0;
volatile bool g_lastCtrlAccept = false;

// ---- which controller is ours. A broadcast reaches every dash in range, so at
// a car meet two controllers can be on the air at once, both called
// "OpenHaldex-C6" if neither owner renamed it. The numbers on the glass and a
// tap on the HALDEX panel must both belong to this car, so the first controller
// whose name matches our SSID is remembered by its MAC, in NVS, and everyone
// else is ignored. Guarded by g_mux.
constexpr const char *kPrefsNamespace = "ohxdash";
constexpr const char *kPrefsCtrlKey = "ctrlMac";
uint8_t g_ctrlMac[6] = {};
bool g_ctrlKnown = false;
volatile bool g_ctrlDirty = false;  // changed: poll() saves it, never the WiFi task

// The last other controller heard, so poll() can mention it once per MAC.
uint8_t g_otherMac[6] = {};
char g_otherName[25] = {};
volatile bool g_otherNew = false;

uint32_t g_lastSeqFrom[4] = {0};    // per-sender: seq of the previous packet
uint8_t g_senderMac[4][6] = {};
int g_senderCount = 0;

// Set by the callback, printed from poll() - never print from the callback,
// it runs in the WiFi task and a blocking UART write there is a bad idea.
volatile bool g_dumpRequest = false;
bool g_firstSeen = false;           // guarded by g_mux, never cleared
// Whether we *want* to be listening. Separate from g_enabled so that a
// deliberate setEnabled(false) is not undone by poll()'s retry, which exists
// only to wait for WiFi to come up.
bool g_want = true;
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
  memcpy(out.ackSrc, st.ackSrc, 2);
  out.ackSeq = st.ackSeq;
  out.ackResult = st.ackResult;
  out.valid = true;

  // Is that ack ours? The controller reports the last two bytes of the MAC
  // that sent the command, and only the last command it accepted, so a match
  // on both means this packet carries the verdict on what we asked for.
  // The nonce to sign the next command with, and whether the controller says
  // it accepts commands at all. Latched here because this runs on the WiFi
  // task and the UI task must not read the packet structure mid-update.
  g_lastNonce = out.nonce;
  g_lastStatusMs = millis();
  g_lastCtrlAccept = out.ctrlAccept;

  if (g_cmdSeq != 0 && out.ackSeq == g_cmdSeq && out.ackSrc[0] == g_ownMac[4] &&
      out.ackSrc[1] == g_ownMac[5]) {
    g_ackResult = out.ackResult;
    g_ackSeq = out.ackSeq;
    g_ackMs = millis();
    g_ackValid = true;
  }
  (void)len;
}

// ---- receive callback --------------------------------------------------
// Runs in the WiFi task. Validate, decode, count; no printing, no loops
// that can block, and take the spinlock only around the shared state.
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  // ESP-NOW has one receive callback, so the CAN relay's packets arrive here too.
  // They are only taken from our own controller, as the status packets are.
  if (len >= (int)sizeof(CannHdr) && data[0] == CANN_MAGIC0 && data[1] == CANN_MAGIC1) {
    taskENTER_CRITICAL(&g_mux);
    const bool ours = g_ctrlKnown && memcmp(g_ctrlMac, info->src_addr, 6) == 0;
    taskEXIT_CRITICAL(&g_mux);
    if (ours) canrelay::onPacket(data, len);
    return;
  }

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

  // Is this our controller? This comes before decode(), which latches the nonce
  // a command gets signed with: another car's packet must not touch any of it.
  // The packet's name is its AP SSID, cut to 23 characters by the sender.
  const char *name = (const char *)data + offsetof(OhxStatus, name);
  taskENTER_CRITICAL(&g_mux);
  const int slot = senderSlot(info->src_addr);
  g_stats.senders = (uint32_t)g_senderCount;
  if (!g_ctrlKnown && strncmp(name, cfg::get().ssid, 23) == 0) {
    memcpy(g_ctrlMac, info->src_addr, 6);
    g_ctrlKnown = true;
    g_ctrlDirty = true;
  }
  if (!g_ctrlKnown || memcmp(g_ctrlMac, info->src_addr, 6) != 0) {
    g_stats.ignored++;
    if (memcmp(g_otherMac, info->src_addr, 6) != 0) {
      memcpy(g_otherMac, info->src_addr, 6);
      memcpy(g_otherName, name, 24);
      g_otherName[24] = '\0';
      g_otherNew = true;
    }
    taskEXIT_CRITICAL(&g_mux);
    return;
  }
  taskEXIT_CRITICAL(&g_mux);

  Status s;
  decode(data, (size_t)len, info->src_addr, rssi, ch, s);

  taskENTER_CRITICAL(&g_mux);
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

// Write the remembered controller to NVS, or remove it if it was forgotten.
// Flash work, so this runs from poll() on the main loop and never from the WiFi
// task's receive callback.
void saveController() {
  uint8_t mac[6];
  taskENTER_CRITICAL(&g_mux);
  const bool known = g_ctrlKnown;
  memcpy(mac, g_ctrlMac, 6);
  taskEXIT_CRITICAL(&g_mux);

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) return;
  if (known) {
    prefs.putBytes(kPrefsCtrlKey, mac, 6);
    Serial.printf("[now] following controller %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1],
                  mac[2], mac[3], mac[4], mac[5]);
  } else {
    prefs.remove(kPrefsCtrlKey);
    Serial.println("[now] controller forgotten: following the next one with our SSID");
  }
  prefs.end();
}

}  // namespace

void loadController() {
  uint8_t mac[6];
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) return;
  const bool have = prefs.getBytesLength(kPrefsCtrlKey) == 6 &&
                    prefs.getBytes(kPrefsCtrlKey, mac, 6) == 6;
  prefs.end();
  if (!have) return;

  taskENTER_CRITICAL(&g_mux);
  memcpy(g_ctrlMac, mac, 6);
  g_ctrlKnown = true;
  taskEXIT_CRITICAL(&g_mux);
  Serial.printf("[now] remembered controller %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1],
                mac[2], mac[3], mac[4], mac[5]);
}

bool controller(uint8_t mac[6]) {
  taskENTER_CRITICAL(&g_mux);
  const bool known = g_ctrlKnown;
  memcpy(mac, g_ctrlMac, 6);
  taskEXIT_CRITICAL(&g_mux);
  return known;
}

void setController(const uint8_t mac[6]) {
  taskENTER_CRITICAL(&g_mux);
  memcpy(g_ctrlMac, mac, 6);
  g_ctrlKnown = true;
  g_ctrlDirty = true;
  taskEXIT_CRITICAL(&g_mux);
}

void forgetController() {
  taskENTER_CRITICAL(&g_mux);
  g_ctrlKnown = false;
  g_ctrlDirty = true;
  taskEXIT_CRITICAL(&g_mux);
}

bool begin() {
  if (g_enabled) return true;
  g_want = true;
  if (WiFi.getMode() == WIFI_MODE_NULL) return false;  // WiFi must be started first

  const esp_err_t err = esp_now_init();
  if (err != ESP_OK) {
    Serial.printf("[now] esp_now_init failed: %d\n", (int)err);
    g_readyAtMs = millis();
    return false;
  }
  esp_now_register_recv_cb(onRecv);
  WiFi.macAddress(g_ownMac);  // to tell our own ack from anyone else's

  // A peer entry is only needed to *send*. The broadcast peer is registered
  // here so the peer table is exercised from boot, and a command to a specific
  // controller (sendMode) adds that controller's unicast peer on demand.
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
  if (g_ctrlDirty) {
    g_ctrlDirty = false;
    saveController();
  }
  if (g_otherNew) {
    taskENTER_CRITICAL(&g_mux);
    g_otherNew = false;
    uint8_t mac[6];
    char name[25];
    memcpy(mac, g_otherMac, 6);
    memcpy(name, g_otherName, sizeof(name));
    taskEXIT_CRITICAL(&g_mux);
    Serial.printf("[now] ignoring %02X:%02X:%02X:%02X:%02X:%02X \"%s\": not this dash's controller\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], name);
  }

  if (!g_enabled && g_want) {
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
  g_want = on;
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
  g_readyAtMs = millis();  // a re-enable waits one retry interval, as at boot
  Serial.println("[now] listener off");
  return true;
}

bool sendMode(uint8_t mode) {
  if (!g_enabled || mode > 5) return false;
  // Nothing to sign against until the controller has spoken: the nonce in the
  // packet is part of what makes the signature single-use.
  if (g_lastStatusMs == 0 || (uint32_t)(millis() - g_lastStatusMs) > 2000) return false;
  // A command is a message to one car: ours, and only ours.
  uint8_t target[6];
  if (!controller(target)) return false;

  OhxCommand c = {};
  c.h.magic[0] = OHX_MAGIC0;
  c.h.magic[1] = OHX_MAGIC1;
  c.h.type = OHX_T_COMMAND;
  c.h.proto = OHX_PROTO;
  c.nonce = g_lastNonce;
  c.seq = ++g_cmdSeq;
  c.cmd = OHX_C_MODE;
  c.arg[0] = mode;

  // HMAC-SHA256 over everything before the hmac field, keyed with the
  // controller's access-point password - the same string the station joins
  // with. An open access point means an empty key, which is a key, not the
  // absence of one; see the warning in the settings page.
  const char *key = cfg::get().pass;
  uint8_t full[32];
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(md, (const uint8_t *)key, strlen(key), (const uint8_t *)&c,
                  offsetof(OhxCommand, hmac), full);
  memcpy(c.hmac, full, 8);

  // Unicast to our controller. The broadcast peer is registered for sending,
  // but a command is a message to one car.
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, target, 6);
  peer.channel = 0;  // follow the station's channel
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  const esp_err_t perr = esp_now_add_peer(&peer);
  if (perr != ESP_OK && perr != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("[now] command peer add failed: %d\n", (int)perr);
    return false;
  }

  g_ackValid = false;  // this command is the one the next ack can be about
  const esp_err_t serr = esp_now_send(target, (const uint8_t *)&c, sizeof(c));
  g_lastCmdMs = millis();
  if (serr != ESP_OK) {
    Serial.printf("[now] send failed: %d\n", (int)serr);
    return false;
  }
  return true;
}

Ack lastAck() {
  Ack a{};
  a.valid = g_ackValid;
  a.result = g_ackResult;
  a.seq = g_ackSeq;
  a.ageMs = g_ackValid ? (uint32_t)(millis() - g_ackMs) : UINT32_MAX;
  return a;
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
                "age %lu ms, %lu sender(s), rssi %d)   -- the glass is showing: %s\n",
                (unsigned long)st.packets, (unsigned long)st.rejects, (unsigned long)st.duplicates,
                (unsigned long)st.gaps, (double)st.hz,
                st.ageMs == UINT32_MAX ? 0UL : (unsigned long)st.ageMs,
                (unsigned long)st.senders, (int)st.rssi, feed::sourceName());
  if (!s.valid) Serial.println("  (no ESP-NOW packet received yet)");
  Serial.printf("  %-11s %10s   %10s\n", "field", "ESP-NOW", "HTTP");
  row("rpm", s.rpm, l.rpm, "");
  row("speed", s.speed, l.speed, "km/h");
  row("throttle", s.throttle, l.throttle, "%");
  row("boost", s.boost, l.boost, "mbar");
  row("lockTarget", s.lockTarget, l.lockTarget, "%");
  row("lockActual", s.lockActual, l.lockActual, "%");
  row("coolant", NAN, l.coolant, "C  (HTTP only)");
  row("engine oil", NAN, l.oil, "C  (HTTP only: Kombi_2)");
  row("haldex oil", s.oilTemp, NAN, "C  (ESP-NOW only: Gen4 over KWP)");
  row("clutch", s.clutchTemp, NAN, "C  (ESP-NOW only)");
  row("module", s.moduleTemp, NAN, "C  (ESP-NOW only)");
  row("supply", s.supplyV, NAN, "V  (ESP-NOW only)");
  // The one field the two transports disagree about on purpose, and the only
  // reason this row exists: /api/dashboard gates steeringAngle on the Haldex
  // generation (2/4/50/52) while the packet carries it for any car whose
  // chassis bus sends LW_1 / LWI_01 - which includes Gen1. Printing them side
  // by side is how that gets reported rather than argued about.
  row("steering", s.steering, d.steeringAngle, "deg  (HTTP null on Gen1)");
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
