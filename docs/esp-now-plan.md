# Plan: take the live data over ESP-NOW instead of HTTP

Status: **proposal, nothing implemented yet.** Written so the steps and the
risks are reviewable before any code changes.

## 1. What ESP-NOW is, in plain terms

Every ESP32 has a WiFi radio that can do three things:

* be an access point (the OpenHaldex-C6 does this),
* be a station that joins an access point (what this dash does today),
* do **ESP-NOW**, which is a peer-to-peer mode built into the same radio.

ESP-NOW is closer to walkie-talkies than to networking. There is no access
point, no association, no IP address, no DHCP, no TCP, no HTTP. You put a fixed
6-byte address on a packet and push it at the radio; the other side's radio
hands it to your callback. Packets are up to **250 bytes**, delivery is
typically **1-2 ms**, and a packet can be addressed to the broadcast address so
*anyone listening on the channel* gets it.

Consequences that matter here:

* No association and no socket means **none of the WiFi stack runs**, so none
  of the memory traffic that is currently starving the display.
* Nothing to reconnect: if the C6's access point comes and goes (it does, on a
  mode change), the dashboard doesn't care.
* It is a radio broadcast, so there is **no retransmission**. A lost packet is
  simply lost, which is fine because the sender repeats 10 times a second.

## 2. What the OpenHaldex-C6 already does

Read from `Forbes-Automotive/OpenHaldex-C6` (`src/OpenHaldexC6_ESPNow.cpp`
and `include/ohx_proto.h`). It is **already sending everything we need**, and
enabled by default (`espNowEnabled = true`):

* Every **100 ms** it broadcasts a packed `OhxStatus` struct to
  `FF:FF:FF:FF:FF:FF` — no handshake, no registration, any listener can
  decode it.
* Header: 2 magic bytes, a type byte, and `OHX_PROTO` (currently 1).
* Payload includes: `rpm`, `speed`, `boost` (mbar), `throttle`, `lockTarget`,
  `lockActual`, `engagementRaw`, `haldexState`, `mode`, four flag bytes
  (standalone / chassis CAN / haldex CAN / override / learned ...),
  **`slip[4]` per-wheel slip**, `steering` angle, `clutchTemp`, `oilTemp`,
  `moduleTemp`, **`supplyMv` battery voltage**, learn progress, plus the C6's
  own **WiFi channel** and SSID.
* It can also *receive* commands to change mode, signed with an 8-byte
  HMAC keyed with the AP password (`OhxCommand`, 22 bytes).

That is **more data than the HTTP API** gives us, at 10 Hz, with no HTTP.

Not in it: **coolant temperature** (chassis CAN only), and anything to do with
settings. Those stay on HTTP.

## 3. Why this is the right next move

Measured on this board today:

| Situation | Render rate |
|---|---|
| Link up, polling paused | 42 fps (pinned to the panel) |
| Link up, `/api/live` at 10 Hz | 16-27 fps |

So an **idle association costs nothing**; what costs the frame rate is the
request traffic itself (each request allocates and frees packet buffers that
the prebuilt IDF libraries place in PSRAM, which is the same memory the panel
is streaming its frame buffer out of).

That gives the plan its shape: move the 10 Hz firehose to ESP-NOW, keep a
*small* amount of HTTP (1 Hz) for coolant and for anything that needs a web
request. Then re-tune the panel clock with the bandwidth that frees up.

## 4. Steps

### Phase 0 — pin down the contract (no S3 code)
1. Freeze the `OhxStatus` field order and `OHX_PROTO` version we are coding
   against, and record the sentinel values (`OHX_NONE8`, `OHX_NONE16`,
   `OHX_NONE_T`) that mean "no data" — they map to the `NAN`/unknown handling
   the UI already has.
2. Decide the channel strategy: the C6 broadcasts on whatever channel its
   radio is on. The packet carries `channel`, so the dash can follow it, but
   it needs a starting point (one scan at boot, or a configured default).

### Phase 1 — a listen-only receiver, behind a build flag
Nothing changes on screen; HTTP stays the source of truth.

3. New `src/net/ohx_now.{h,cpp}`: mirror `OhxStatus` as a `__attribute__((packed))`
   struct with `static_assert` on its size, so a layout mismatch is a compile
   error rather than corrupt numbers.
4. `esp_now_init()`, a receive callback, and a broadcast peer entry; the radio
   set to the C6's channel (`esp_wifi_set_channel`). All of this is in the
   IDF already linked into this build — no new dependency.
5. Sanity-filter every packet: magic, type, proto version, minimum length.
   Anything else is counted and dropped.
6. Log the first packet as hex once, so the layout can be confirmed against a
   real C6 on the bench before trusting it.
7. Counters in the 5-second stats line: packets/s (expect ~10), sequence gaps,
   channel, RSSI, and the age of the newest packet.
8. A serial command to print ESP-NOW and HTTP values side by side. The bench
   cross-check is pressing the mode button on the OpenHaldex unit and watching
   `mode`/`lockTarget` change in *both* sources at once.

### Phase 2 — make ESP-NOW the source for the fast values
9. Feed `rpm/speed/boost/throttle/lock*/slip/steering/temps/supply` from the
   ESP-NOW packet; keep coolant from HTTP.
10. Source selection: use ESP-NOW while packets are fresh (< ~500 ms), fall
    back to HTTP if they stop, and say which source is in use in the status
    bar / stats line rather than silently mixing.
11. Drop `/api/live` from 10 Hz to 0, and keep `/api/dashboard` at 1 Hz (it is
    what tells the C6's web stack that someone is still there — it drops its
    own access point after a few minutes of web silence).
12. Re-tune: with the PSRAM freed up, walk the pixel clock back up
    (16 -> 20 -> 24 MHz, i.e. 33 -> 41 -> 49 Hz refresh) and re-measure
    fps, `ms/wait` and stability.
13. Point the UI's stale detection at the ESP-NOW packet age.

### Phase 3 — optional, later: commands from the dash
14. Mode changes from the touchscreen: build the 22-byte `OhxCommand` with the
    nonce from the newest status packet and the 8-byte HMAC. Note the key is
    the AP password — which is empty by default, so with the default setup the
    signature is over an empty key. Worth deciding deliberately.
15. Add the buttons to the settings page only after read-only is solid.

## 5. Risks and how they are handled

| Risk | Handling |
|---|---|
| Wrong channel = total silence | one scan at boot for the SSID's channel, then follow the `channel` field; log every channel change |
| The C6's radio leaves the channel | its own firmware notes that joining a home network pulls the AP off-channel briefly; tolerate gaps, log them, keep the HTTP fallback |
| No retransmission on broadcast | sender repeats at 10 Hz; a gap shows up as one stale frame, not a stuck screen |
| Power save would miss packets | modem sleep must stay off (it is off in this build) |
| Silent data corruption if the struct changes | packed struct + `static_assert` + proto version check; drop and count mismatches |
| Losing HTTP-only values | coolant and settings writes stay on HTTP deliberately |
| No C6 changes wanted | none needed — it is already broadcasting |

## 6. What we would need to decide

* Do we keep the WiFi association at all (for coolant at 1 Hz) or go fully
  stationless? Recommendation: keep it — an idle association measured free.
* How much of the settings UI moves to ESP-NOW later (safe read-only first).
* Whether the battery voltage / per-wheel slip / steering angle should earn
  space on the dashboard, since they arrive for free.
