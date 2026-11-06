# TODO

What is deliberately not built, not verified, or not decided. Kept separate
from the README so the README can describe what *is*, and this can describe
what *might be*, without either one lying.

Ordering inside each group is roughly "what would hurt most if it bit us"
downwards.

---

## 1. Waiting on a decision

### 1.1 Rescue access point
If the dash's SSID or password is set wrongly, it cannot join the controller's
access point — so it cannot fetch data, and the over-the-air update page is
unreachable. USB is the only way back.

The fix: if the station has failed to join for ~3 minutes, raise our own access
point (`ohx-dash-rescue`), put the address on the glass, and serve the same
update page plus a way to fix the stored credentials. Roughly a softAP, a
small DHCP server and one more page; maybe 60–80 KB of flash.

**Deferred, not rejected.** An edge case you have to walk into on purpose.
Worth doing if this ever ships to someone who cannot reach the USB port.

### 1.2 Authenticate the update page
Right now anyone on the controller's access point can flash the dash. That is
a different risk class from the mode switch — a stranger changing the Haldex
map is annoying, a stranger writing your firmware is not — and the open access
point is broadcast, so "who is on it" is everyone in range.

Cheapest fix: a 6-digit code in the URL, shown on the dash's SETTINGS page next
to the address. Not a login, just enough that the page cannot be *found* by a
casual passer-by. Alternatives: leave it (consistent with treating an open
access point as a warning, which is the current stance on the mode switch), or
require the AP password as a token.

### 1.3 Build a `firmware.factory.bin` release asset too?
Releases carry `firmware.bin` (what the update page wants). A board that has
never been flashed, or one whose app slots have been scrambled, needs the
factory image written at `0x0` instead. One line of release notes, or a second
asset — decide when the first person asks.

---

## 2. Verification gaps

Things built and believed, but not yet proven on the right hardware.

### 2.1 Other Haldex generations — **needs testers**
Everything layout-related was decided from a **Gen1** car. Gen1 has no boost
frame, no supply voltage, no Haldex temperatures and a chassis-CAN-only
handbrake, so:

* the **Full (B)** tile band (`SPEED | BOOST | TEMPERATURES | HALDEX`) has
  never been rendered on a car that fills all four panels;
* `boost` scaling is the one field nobody has ever seen move — the packet
  documents it as *mbar above ambient*, and that is an assumption until a
  Gen4/5 car proves it;
* the capability table's "table" half (boost from generation) has only ever
  been exercised on the car that *lacks* the source;
* Gen2/4/50/52 differences still unknown: which generations fill `slip[4]`,
  and whether steering arrives both ways as it does on Gen1. (`oilTemp` is
  settled: the packet's is Haldex oil, Gen4 only, and the OIL tile no longer
  uses it. HTTP oil is engine oil from the Kombi.)

The raw telemetry page (`x`) exists for exactly this: it shows every field
raw, so a tester can report back without a laptop and without this agent.

### 2.2 The CAN relay in the car
Built and believed, not yet measured. What it costs in render rate, how much it
loses in the air at this car's −76 to −86 dBm (the controller sends at 6 Mbit/s
while relaying, which has less margin than the 1 Mbit/s the link was measured at),
and which relay setting to recommend. MQB coolant and oil from `Motor_07` have
never been on a car through this path. The whole test is in
[`docs/can-relay-test.md`](docs/can-relay-test.md).

### 2.3 A wrong *project* image is accepted
The IDF refuses an image for another chip (verified in its source:
`mismatch chip ID, expected %d, found %d`, on the same verify path
`Update.end()` uses). It does **not** care whose project the image is, so a
valid ESP32-S3 image that is not this dashboard would be written, would boot,
and would never stand the trial down — leaving a trial armed in NVS for the
next real boot to trip over.

The fix, in `handleUploadDone()`: after `Update.end(true)`, read the app
descriptor out of the target partition (`esp_partition_read` at
`16 + 8 * segment_count`, rounded up) and compare `project_name` with this
project. On a mismatch, point the boot partition back at the running slot and
report it. The running image is untouched either way.

**Not done because it cannot be tested from here** — the update page is only
reachable from the controller's access point, and the controller's access
point is out of range of the development machine. Until then the README says
plainly: take the file from this repository's releases and nothing else.

### 2.4 The unexplained reset
One reset, 8 s after the first post-update boot, while the station was
re-associating. No panic, a clean ROM banner, no repeats in 180 s of idle
running afterwards, and the trial logic handled it correctly in passing.

Leading hypothesis: a supply sag. The panel and the WiFi share a rail with
very little margin on a USB port, and the reset landed in the middle of
association retries. The other candidate has since been removed: until 0.4.2 a
rejoin shut the WiFi driver down with ESP-NOW still running on top of it. The boot banner now prints `reset : <reason>`
(power-on / software / PANIC / watchdog / BROWNOUT), so the next occurrence
answers the question instead of prompting another guess. Watch it on the car's
supply, which is a different animal.

### 2.5 Link margin
The ESP-NOW link sits at −76 to −86 dBm in the car and delivers 6–9 Hz rather
than the full 10, with occasional gaps. Nothing is wrong with the code —
broadcast has no retransmission — but the dash would look better with the S3
moved ~20 cm away from whatever it is currently sitting next to.

Worth trying when someone has the car apart: a position that gets to −70 dBm,
and measuring `[stats] esp-now … Hz` before and after.

### 2.6 Internal RAM changes (0.5.1) on a real panel
Written and not yet run: there was no panel to hand. With the glass in front of you,
compare against 0.5.0:

* `[stats] render ... fps`, on the dashboard page with the relay on. LVGL's widgets
  are now in PSRAM, which the panel is already short of. If it drops noticeably, set
  `DASH_LVGL_POOL_IN_PSRAM` to 0 in `include/lv_conf.h`.
* The picture stays steady, including while the CAN DATA and SETTINGS pages open.
* An over-the-air update and a settings save still work.
* `[ram] ... lowest` after 10 minutes in the car, with the relay on and a phone on the
  update page. Then `[ram] lvgl pool ... peak`: the pool can be cut to suit.

If it is still short, in order: cut `LCD_BOUNCE_LINES` from 24 to 16 (26 KB, needs the
glass to judge), then trim the `ohxnet` stack using `stack spare` (it must stay
internal, because the over-the-air update writes flash from that task). Rebuilding the
framework as can2gauge did would gain little: WiFi and lwIP's speed-ups are already out
of internal RAM in this build, and that rebuild does not currently succeed here (§4.1).

---

## 3. Requested, and worth doing

### 3.1 Choose your own boot splash
At the moment the panel is blank for the second between power and the first
frame. A splash could cover that and say something useful: version, build
date, board, the update address, maybe "waiting for the controller".

Design questions to settle first: is it a static image compiled in, or a file
on the unused 128 KB LittleFS partition; does "choosable" mean a setting on
the device, or at build time; and does it stay up until the first data arrives
(which is also a nice "the controller is not talking to me" indicator).

Cost: an LVGL image widget plus a decoded image in flash. The partition has
room; the app slot has ~290 KB.

### 3.2 Display scaling / other panel sizes
Currently out of scope, and worth stating precisely why: `theme.h` is a
list of absolute pixel positions derived from the browser page's geometry
(`u = min(w/100, h/56)`, 8 px at 800×480). Nothing scales.

The **5.0" CrowPanel** is the same 800×480, so it should fit as-is and needs
only a board variant — different pins, touch controller and possibly less
PSRAM, which matters more than the resolution: the two 800×480×16bpp
framebuffers are 1.5 MB and cannot live in internal RAM. Check the 5" board's
PSRAM *before* anything else.

For a genuinely different resolution, the honest options are (a) express
`theme.h` in terms of a scale factor and multiply, (b) convert the layout to
LVGL flex/grid, or (c) accept per-resolution variants. (a) is the smallest
change and keeps the "one file holds the geometry" property.

### 3.3 Internal SD / logging
A short CSV log of a drive — rpm, speed, throttle, boost, locks, mode over
time — written to the LittleFS partition and dumped over serial or from the
update page. That would turn "the boost reading looks wrong" into a graph, and
it is the kind of thing that makes a drive test worth three drive tests.

### 3.4 More of the controller's own controls
The mode switch is the thin end of a wedge. The same signed-command path
already carries learn start/cancel/clear, generation, and controller on/off
(see `ohx_proto.h`). Each one is a decision, not a port: generation and learn
need the car stopped, and "controller off" while driving is a conversation.

### 3.5 Things that arrive for free and have nowhere to go
Per-wheel slip (`slip[4]`), steering angle (packet-only on Gen1), supply
voltage, Haldex clutch/oil/module temperatures. All decoded, all shown on the
raw page, none on the dashboard — because this car cannot fill them. Space
them once a Gen4/5 tester exists (see 2.1).

### 3.6 Smaller polish
* Scale up the big SPEED number — noted as not urgent, since speed matters
  least on a racecar-style dash, but it is inconsistent with rpm.
* Imperial speed (mph) alongside the existing °F and psi.
* A QR code on the SETTINGS page for the update URL, to kill the last bit of
  typing on a phone.

---

## 4. Technical debt

### 4.1 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
The IDF's own rollback is the right home for the update trial. Enabling it
means `custom_sdkconfig`, which forces a full framework rebuild against the
shared core directory — measured, and it does not currently succeed for this
platform release (`libsodium` fails to compile once the regenerated sdkconfig
drops its defines). Revisit if upstream fixes the rebuild; the application's
trial can be deleted the same day.

The application-level version cannot catch a crash that happens before our
code runs. The IDF's version can.

### 4.2 The `coredump` partition is unused
It exists in the partition table and nothing writes to it. Enabling core
dumps, and a way to read one back out (over the update page would be neat),
would turn a field crash from "it rebooted" into a stack trace.

### 4.3 Render performance
16–35 fps depending on what the radio is doing; the loop is PSRAM-bandwidth
bound, not HTTP bound (see §8 of the README). The pixel clock is deliberately
left where it is known to be stable. The `p` and `b` console keys remain the
fastest way to explore it if anyone wants more.

### 4.4 Report upstream: the steering inconsistency
`/api/dashboard` gates `steeringAngle` on `steerSupported = (gen == 2 || 4 ||
50 || 52)`, so on Gen1 the JSON field is `null` — while the ESP-NOW packet
carries the same `received_steering_angle` for the same car, and so does the
BLE characteristic. The CAN decoder supports PQ `LW_1` (0x0C2) *and* MQB
`LWI_01`, so the platform is deliberately supported one layer down and then
excluded at the API. It shows in the controller's own web UI too
(`data/app.js` renders `steeringAngle`), and `steeringHealthy` is `false` on
Gen1, so a client that trusts the health flag is misinformed rather than
merely uninformed.

The dash uses the packet and is unaffected; the raw page and `e` on the
console show both transports side by side. The evidence, the three-path
comparison and a suggested fix are written up ready to paste in
[`docs/upstream-steering-report.md`](docs/upstream-steering-report.md).

### 4.5 `ohx_proto.h` and `cannow_proto.h` must be refreshed from upstream
They are verbatim copies of the controller's shared headers. If the C6's struct
changes and this copy does not, the `static_assert` on the packet size will
catch it — but only if someone looks. Worth a note in the release checklist.

### 4.6 LittleFS partition
128 KB, allocated, unused. Either it earns its place (3.1 splash, 3.3 logging)
or it should be given back to the app slots.
