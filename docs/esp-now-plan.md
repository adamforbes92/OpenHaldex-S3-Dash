# Plan: take the live data over ESP-NOW instead of HTTP

Status: **Phase 1 implemented and verified on the bench** (see §7). Phases
2-3 are still proposals. Written so the steps and the risks are reviewable
before any code changes.

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

---

## 7. Phase 1 as built (what actually happened)

Delivered as a listen-only receiver that cannot affect the glass.
**HTTP is still the only source of truth**; nothing on screen comes from
ESP-NOW yet.

### Files

| File | Role |
|---|---|
| `include/ohx_proto.h` | **Verbatim copy** of the C6's shared wire-format header (magic, types, sentinels, flags, structs). Taken from `Forbes-Automotive/OpenHaldex-C6 @ b54d9a1d`, sha256 `9f1a6351c8b5fe34...`. Do not edit it: refresh it from upstream. |
| `src/net/ohx_now.{h,cpp}` | The listener: init, receive callback, decode, counters, printing. |
| `platformio.ini` | `-DOHX_USE_ESP_NOW=1` switches the calls in `main.cpp` in or out. |
| `src/main.cpp` | `ohxnow::poll()` in the loop, an `[stats] esp-now ...` line, and the `e` / `E` console commands. |

Re-typing the struct was the plan's step 3; taking upstream's header instead
is strictly better, because the layout is then guaranteed identical rather
than merely asserted similar. Upstream already keeps two byte-identical copies
of this file in its own repo, so a third copy is the intended pattern.

### Verified on the bench (no car attached, so no CAN data)

```
[now] listening: peer added (0), own mac 44:1B:F6:8B:07:98, station channel 1
[now] first OHX_STATUS: 84 bytes from DC:1E:D5:82:8B:79 rssi -30 dBm, channel 1 (station is on 1)
  header  : magic OH type 1 proto 1, fw 11.0.4, generation 1
  seq 129  nonce 88337fab  name 'OpenHaldex-C6'  struct 84 bytes (ours 84)
  flags   : CTRL_ACCEPT
  mode    : 4 (75:25), haldexState 0
```

* **Layout confirmed byte for byte.** The raw hexdump was walked against the
  upstream header field by field: every offset, sentinel (`0xFFFF`, `0xFF`,
  `0x8000` for temperatures, `-128` for slip) and the `name` string landed
  where the header says. `sizeof(OhxStatus)` is 84 on both sides.
* **Rate and loss:** 9.7-10.6 Hz sustained (the C6's period is 100 ms), 0
  rejects, 0 duplicates, 0-3 sequence gaps per ~500 packets (~0.5%, expected:
  broadcast has no retransmission).
* **Channel strategy needed no scan after all.** We are associated to the
  C6's own access point, so the station is already on its channel; the peer is
  registered with channel `0` ("use the station's channel") and packets arrive
  with `channel 1`, exactly matching `WiFi.channel()`. A boot scan is only
  needed if we ever stop being associated.
* **Listening is free.** A/B on one boot with the `E` toggle: 16.2-16.4 fps
  with the listener on, 16.4 fps with it off, and the new
  `%.2f ms/busy of a %.1f ms period` counters show ~60.2 ms of a 61.0 ms
  period either way - the loop is entirely LVGL render plus the vsync wait.
  ESP-NOW does not starve the panel.
* **Cross-check with HTTP works.** Pressing the mode button on the OpenHaldex
  unit moved `mode` in *both* sources together (`3` and `4` seen at different
  times), and `lockTarget` agreed at `0.00`. `haldexState` differs in meaning
  rather than value: the packet carries the raw Haldex status byte (`0`),
  while the HTTP API reports `-1` for "unknown" when no CAN is present.

### The one thing the bench cannot settle

There is no CAN on the bench, so **every fast value arrives as a sentinel**
(rpm/speed/boost/throttle are `none` on both ESP-NOW and HTTP) and
`clutchTemp`/`oilTemp`/`moduleTemp`/`supplyMv` never get a real reading
either. Their *scaling* is documented upstream (`tenths of a degree`,
`mV`, `mbar above ambient`) but has not been seen on real data here. That is
what `e` is for: in the car, `[now] ESP-NOW vs HTTP` puts the two transports
side by side field by field, and any disagreement is visible before Phase 2
lets the packet drive the glass.

### Incidental find worth keeping

The first-packet decode was originally re-armed whenever the flag was clear,
so it printed for *every* packet. Twenty lines of hex at 115200 baud is ~87 ms
of blocking UART per packet, which dragged the render rate from 16.4 to
6.6 fps. It looked exactly like "ESP-NOW is expensive". Two fixes: dump once
per boot (`g_firstSeen`), and the `ms/busy` counters above, which immediately
showed the loop was 100% busy and so the cost was not in the loop at all but
in serial output the loop was calling.


---

## 8. Phase 2 as built (ESP-NOW feeds the glass)

New file: `src/net/datafeed.{h,cpp}`, the one place that decides where a
number came from. The UI asks it for `live()`, `dash()` and `fresh()`; it
never touches `telemetry::` or `ohxnow::` itself any more.

### The rules, in full

| Rule | Why |
|---|---|
| Source = ESP-NOW while a packet has arrived within `kHoldMs` (500 ms), else HTTP | 500 ms is five missed packets: long enough not to flip on one dropped frame, short enough to switch before the screen looks wrong |
| When ESP-NOW is the source, take every field the packet carries; for a numeric field marked "no data", use the HTTP value if HTTP has one | A field this Haldex generation never fills in (e.g. `oilTemp` is a Gen4 field) must not blank a tile that HTTP was filling |
| Coolant always from HTTP | Not in the packet at all |
| `tcOn` / `asrOn` / `awdWarning` always from HTTP | The packet has `OHX_F1_TC_FORCE` and `OHX_F3_ASR_OFF`, which are *off* flags, not the same thing as the API's lamp states. Mapping them by guessing would have been the one silent way to make the dashboard lie |
| Stale = neither transport has delivered within `staleMs` | The screen is only stale when nothing is current |

Consequences worth noting:

* `brake`, `handbrake`, `couplingOpen` and `tempProtection` now update at
  10 Hz instead of 1 Hz — the brake lamp is 900 ms quicker than it was.
* The status light's `NO WIFI` now means *neither* transport is alive, so the
  dashboard can read `LIVE` while the station is not even associated.
* `/api/live` drops from 10 Hz to 1 Hz (it is now only coolant plus the
  fallback), which also takes ~90 % of the HTTP CPU load off the C6. A config
  version bump (3 → 4) migrates an existing settings blob rather than
  discarding it, so the learnt `netUseStatic` survives.

### Verified on the bench (all three paths)

```
[feed] values now come from esp-now (change 1)      <- before the WiFi join finishes
[ui] LIVE (data age 21 ms, source esp-now)          <- while [stats] says "http down"

E  -> [now] listener off
[feed] values now come from http (change 2)         <- 500 ms later, still LIVE
[stats] source http (packet age 590 ms, http up)

n  -> [net] polling PAUSED
[ui] STALE (data age 1529 ms, source http)          <- neither transport: amber light, values held
n  -> [ui] LIVE (data age 39 ms, source http)
E  -> [feed] values now come from esp-now (change 3)
```

### One thing that did not happen

Moving the 10 Hz firehose off HTTP did **not** raise the render rate: 16.4 fps
before, 16.4 fps after, at the same 16 MHz pixel clock. That is worth
understanding before the next tuning step. The loop is ~60.6 ms busy out of a
61.0 ms period, of which only ~14 ms is the panel flush — the rest is LVGL
drawing in software into PSRAM. And 16.4 fps is exactly half of the panel's
33 Hz, i.e. the work no longer fits inside one panel frame, so each frame
costs two. Reducing the request *rate* (10 Hz -> 1 Hz) changed nothing, which
says the cost sits in the WiFi stack being associated at all, not in our
requests: with the link down the same build measured 42-49 fps.

So the next performance lever is the render cost or the frame period, not the
HTTP traffic — and the `p` / `b` console commands are still the fastest way to
measure it. That is deliberately left alone for now: the instruction was to
keep the pixel clock where it is known to be stable.
