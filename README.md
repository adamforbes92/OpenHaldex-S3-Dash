# OpenHaldex S3 Dash

Native dashboard firmware for a **7" ESP32-S3 panel** that displays the
OpenHaldex-C6 race-car dashboard. It replaces the browser client
(`dash.html` hosted on the C6) with firmware running on the display itself.

**The OpenHaldex-C6 firmware is not modified.** This project is purely a
consumer of the existing, released HTTP API, so the C6 can keep taking
upstream updates.

---

## 1. Hardware

| | |
|---|---|
| Board | Elecrow **CrowPanel 7.0"** HMI, `ESP32 Display-7.0` |
| MCU | ESP32-S3 QFN56 rev v0.2, dual core @ 240 MHz |
| PSRAM | 8 MB **OPI** (octal) — mandatory, see §4 |
| Flash | 4 MB quad |
| Panel | 800x480 RGB565 parallel (16 data lines + DE/VSYNC/HSYNC/PCLK) |
| Touch | GT911 capacitive, I2C on SDA 19 / SCL 20 |
| Expandable | PCA9557 I2C expander: IO0 = panel reset, IO1 = input |
| Backlight | GPIO 2 |
| USB | CH340 UART bridge (`/dev/ttyUSB0`), 460800 baud ceiling |
| MAC | `44:1b:f6:8b:07:98` |

The board was identified read-only, without flashing, from `esptool
flash_id` plus the strings in the factory application partition.

### Board variant

The CrowPanel **5.0"** uses the *same* 16 GPIOs but wired to the panel's
RGB inputs in a **different order**, and swaps DE/VSYNC. If this firmware is
ever built for a 5.0" board:

```ini
build_flags = -DDASH_USE_50_PINMAP=1
```

`include/board_config.h` then swaps in that board's bus description. A wrong
pin map shows up immediately as a garbled / colour-swapped image, and damages
nothing.

---

## 2. What it looks like

The top band is **not** a port of `dash.html`: the tach ring, needle and ticks
were replaced by one segmented bar and a large number, because on this board
the ring was the entire frame budget (`docs/redesign-bars.md`). The four tiles
below it still follow the page's geometry formula:

```
u = min(width / 100, height / 56)          -> 8 px at 800x480
```

```
+------------------------------------------------------------------+
| [###############################################.............]  |  REVS
|  1    2    3    4    5    6    7                        x1000    |  full width
|                         6800                                     |  big number
+---------+----------+--------------+-----------------------------+
| SPEED   | BOOST    | TEMPERATURES | HALDEX                      |
| 123 km/h| 1.55 bar | COOLANT   92 | EXPERT                      |
| peak 187| [======] | [========]   | coupling closed             |
|         | peak 1.8 | OIL      108 | LOCK REQ  40%               |
|         |          | [=======]    | LOCK ACT  38%               |
|         |          |              | THR [===]                   |
+---------+----------+--------------+-----------------------------+
| * LIVE  BRAKE HAND TC ASR AWD  rpm peak  RESET PEAKS DAY SETTINGS |
+------------------------------------------------------------------+
```

REVS is the only full-width item; everything else is a tile, so no value is
shown twice. Boost is a tile again (value, unit, a small bar, its peak).

Everything that cannot be measured shows `--`, never a fake zero: a JSON
`null` from the C6 is carried all the way to the screen as "unknown".

Features carried over from the page: peak tracking, shift-light flash,
day/night brightness, and a settings page with all the same thresholds.

**Staleness only moves the status light.** When the live data stops arriving
the dashboard keeps the last values it had — the bars stop where they were,
the tiles keep their numbers — and the status dot turns amber with `STALE`
(or `WAIT` before the first sample ever arrives, `NO WIFI` in red when the
link itself is down). Nothing is blanked and no overlay covers the screen; a
value that was *never* received stays at `--`, because NaN is the only thing
that draws a dash. `updateBars` / `updateReadouts` therefore take no `stale`
flag at all, and the only consumer of it is `updateConn`.

**Deliberate deviations from `dash.html`:**

* Status bar is **30 px** instead of 2.8u = 22 px, so the buttons are a
  usable touch target. The bar band absorbs the difference.
* Long-press anywhere on the bar band opens settings, because the status-bar
  buttons are small for use in a moving car.
* Boost / speed / rpm are drawn with the "eased" value (110 ms time
  constant, same as the page) rather than snapping.

---

## 3. Data flow

Two transports feed the dashboard. **ESP-NOW is the primary one** — the C6
broadcasts an `OhxStatus` packet every 100 ms with no web stack involved, so
there is no access point, no sockets and no TCP in that path at all. HTTP is
the fallback, and it still carries the fields the packet does not have.

```
        core 0                                 core 1
+-----------------------------+        +---------------------------+
| "ohxnet" task               |        | loop()                    |
|   WiFi STA -> C6 HTTP API   | struct |   ui::tick()     ~30 Hz   |
|   GET /api/live       1 Hz  | --+    |   feed::poll()            |
|   GET /api/dashboard  1 Hz  |   |    |   lv_timer_handler()      |
| ESP-NOW receive (radio, no  |   +--> |---------------------------|
|   association, no sockets)  |        | net/datafeed.cpp: choose  |
+-----------------------------+        |   source, merge fields    |
      ^  ~10 Hz, no HTTP              | LVGL -> 2x 800x480 PSRAM  |
      |                               | frame buffers -> RGB DMA  |
   C6 broadcasts OHX_STATUS           +---------------------------+
   to FF:FF:FF:FF:FF:FF
```

`net/datafeed.{h,cpp}` is the single place that decides provenance:

| | Source |
|---|---|
| `rpm`, `speed`, `throttle`, `boost`, `lockTarget`, `lockActual`, `oil` | ESP-NOW while packets are arriving (hold 500 ms), else HTTP |
| `mode`, `haldexState`, `chassisCAN`, `haldexCAN`, `brakeIn`, `handbrakeIn`, `couplingOpen`, `tempProtection` | ESP-NOW — the packet has real bits for all of these, so the brake lamp now updates at 10 Hz instead of 1 Hz |
| `coolant` | always HTTP — the packet has no coolant field |
| `tcOn`, `asrOn`, `awdWarning` | always HTTP — the packet has `TC_FORCE` / `ASR_OFF` (off flags), which are not the same thing |
| packets stopped | everything falls back to HTTP, and says so on the console |

A numeric field the packet marks as "no data" (`OHX_NONE*` → NaN) falls back
to the HTTP value if HTTP has one, so a field this Haldex generation never
fills in cannot make the dashboard worse than it was. The source in use is
printed in the stats line and on every change (`[feed] values now come from
esp-now`), and `e` on the console dumps both transports side by side.

| Endpoint | Rate | Fields used |
|---|---|---|
| `/api/live` | 1000 ms | `coolantTemp` — and everything, if the packets stop |
| `/api/dashboard` | 1000 ms | `tcOn`, `asrOn`, `awdWarning` — and everything, if the packets stop |

### Why HTTP is still there at all

Three reasons, all deliberate: coolant temperature is not in the packet; the
three lamps above are not in it with matching semantics; and if the C6 stops
broadcasting (older firmware, `espNowEnabled` off, out of range) the
dashboard keeps working at 1 Hz instead of going dark.

### Why two rates

`/api/dashboard` on the C6 calls `getCPUUsagePercent()`, which mallocs and
walks every task on the C6's **single** core. Polling that at 10 Hz would
cost roughly ten times the CPU for no extra visual resolution. Splitting the
fast and slow data keeps the same look for ~90 % less CPU on the C6.

### Why `/api/dashboard` is never slower than 2 s

It is the only handler that calls `otaNoteWebActivity()`. The C6 drops its
access point after ~5 minutes without it. The settings page clamps
`pollDashMs` to 250..2000 ms for exactly this reason.

### Joining the C6's access point

The C6 publishes an **open** access point (`wifiPassword` defaults to `""`),
so two defaults in the Arduino core have to be worked around:

* `WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK)` is the core's default, i.e. an
  open network is *below* the threshold. The station therefore has to be told
  `WIFI_AUTH_OPEN` before `WiFi.begin()`.
* Association and DHCP are separate problems. This board associates fine and
  then never gets a lease, which looks identical to "AP is dead" from
  outside. The reason is that the C6 is usually *also* on a home LAN that
  already uses `192.168.1.0/24` (its softAP and its station interface then
  share a subnet), so its DHCP server cannot hand out an address that looks
  free. The firmware logs `associated but no DHCP lease` and retries with a
  static address derived from the configured host (`192.168.1.203`), which
  skips DHCP completely. This also suits a fixed install: the dash talks to
  one AP at one address, so there is nothing for DHCP to decide.

To tell the two failure modes apart, the join loop prints the BSSID, channel
and RSSI, which only read back once the station is associated.

### Connection reuse

Both pollers use `HTTPClient::setReuse(true)`, so 10 requests/second share
one TCP connection instead of opening ten. That matters: the C6 runs with
`CONFIG_LWIP_MAX_SOCKETS=8`, and a handshake-per-request pattern would fill
its socket table with `TIME_WAIT` entries.

---

## 4. Build

```bash
cd /home/adam/OpenHaldex-S3-Dash
./.venv/bin/pio run -e s3-dash
```

The first build downloads the pioarduino `espressif32@55.3.312` platform
(the same release the C6 firmware uses) plus the framework — a few minutes,
once.

**If the platform disappears on the first build**, the installed core is
reporting itself as a post-release of 6.2.0 (`6.2.0.post1`), which PlatformIO
turns into the semver *pre-release* `6.2.0-post.1`. The platform declares
`engines >= 6.2.0`, the pre-release fails that test, and Platform Manager
deletes the platform — leaving `~/.platformio/platforms` empty and a build log
that ends with `IncompatiblePlatform`. The core is a post-release, not a
pre-release, so report it as plain 6.2.0:
`platformio/__init__.py` → `VERSION = (6, 2, 0)` (this machine has it patched
in both the global `penv` and the project `.venv`, with the reason in a
comment).

**PSRAM must be OPI.** The panel driver needs two 800x480x16 bpp frame
buffers (1.50 MB) reachable by the LCD DMA engine. `platformio.ini` sets
`board_build.arduino.memory_type = qio_opi`; without it `lcd.begin()` fails
and nothing is drawn.

### Flash

```bash
# 1. back up first (read-only, no risk) - see §5
# 2. then, with the board in download mode:
./.venv/bin/pio run -e s3-dash -t upload
```

`upload_speed` is pinned to **460800**. The CH340 on this board fails at
921600 with `Invalid SLIP escape (0xdb, 0xBD)`.

---

## 5. Factory backup

A complete, read-only 4 MB image of the board as shipped is in
`.backup/` (gitignored):

| File | Bytes |
|---|---|
| `.backup/s3_factory_0x0-0x100000.bin` | 1 048 576 |
| `.backup/s3_factory_0x100000-0x400000.bin` | 3 145 728 |
| `.backup/s3_factory_full_4MB.bin` | 4 194 304 |
| `.backup/s3_factory_full_4MB.sha256` | — |

```
sha256  8e1b8aba762ae767b8cb8ec2afe7f22455f4dfe464d06230b74d816d016378e6
```

Restore with:

```bash
./.venv/bin/esptool -p /dev/ttyUSB0 -b 460800 write_flash 0x0 \
    .backup/s3_factory_full_4MB.bin
```

The factory partition table, read out of that image:

| Label | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 0x5000 |
| otadata | data/ota | 0xe000 | 0x2000 |
| app0 | app/ota_0 | 0x10000 | 0x3c0000 |
| spiffs | data/spiffs | 0x3d0000 | 0x20000 |
| coredump | data/coredump | 0x3f0000 | 0x10000 |

This firmware replaces it with `partitions_4mb_ota.csv`, which trades app
size for **two app slots (OTA)**, a 896 KB LittleFS partition and a
coredump partition. OTA matters once the dash is behind the dashboard, where
there is no USB cable.

---

## 6. Configuration

Defaults are the same as `dash.html`:

| Setting | Default |
|---|---|
| SSID / password | `OpenHaldex-C6` / *(empty — the C6 AP is open by default)* |
| Host | `192.168.1.1` (the C6 access-point address; `openhaldex.local` also works) |
| rpm max / amber / redline / shift | 8000 / 6000 / 6800 / 7300 |
| coolant warn / crit (C) | 100 / 110 |
| oil warn / crit (C) | 120 / 135 |
| boost max / warn / crit (mbar gauge) | 2500 / 1800 / 2200 |
| temp units / boost units | C / bar |
| poll live / dash / stale | 1000 / 1000 / 1500 ms |

Stored in NVS (`Preferences`, namespace `ohxdash`) as one versioned blob,
so a firmware update that changes the struct layout falls back to defaults
instead of reading garbage.

**No credentials are in git.** For bench work copy `include/wifi_secrets.h.example`
to `include/wifi_secrets.h` (gitignored) — it is only a first-boot seed; the
settings page always wins afterwards.

### Units

Boost is **millibar gauge** (above ambient). The C6 decodes both MQB
`Ladedruck` (0x107) and PQ (0x588) as `(raw * 10) - 1000` and clamps to
`>= 0`, so 1000 mbar ≈ 1.00 bar ≈ 14.5 psi.

---

## 7. What this device cannot show

Data the C6's HTTP API does not expose, so it is **not** on this dashboard:

* Engine oil **pressure**, fuel pressure, individual wheel speeds.
* Gear position, clutch state (the browser page does not show these either).
* Battery voltage — this *is* available, but only in the `OHX_STATUS`
  ESP-NOW broadcast (`supplyMv`), not over HTTP. The dash now receives it
  (§9); it just has no space on the layout yet.
* Anything at all while `chassisCAN` is false: on the bench with no vehicle
  CAN the C6 reports `rpm`/`speed`/`throttle`/`boost` as JSON `null`, and
  this firmware correctly shows `--`.

---

## 8. Display tuning

The RGB panel configuration in `src/hal/crowpanel.cpp` is Elecrow's own
7.0" description. Two things occasionally need changing on a board that has
been reworked or is a different revision:

| Symptom | Fix |
|---|---|
| Colours swapped (red shows as blue) | build with `-DDASH_USE_50_PINMAP=1`, or permute `kDataPins` |
| Image shifted / wrapped | `LCD_HSYNC_*` / `LCD_VSYNC_*` porches in `board_config.h` |
| Whole image jitters / shifts | panel DMA starved of PSRAM bandwidth: raise `LCD_BOUNCE_LINES`, then lower `LCD_PCLK_HZ` |
| Image tears or rolls | LVGL is drawing into the buffer being scanned: keep `LCD_USE_DOUBLE_FB 1` and `LV_DISPLAY_RENDER_MODE_DIRECT` |
| Panel blank, no backlight | `PCA9557` not found — check the I2C scan line in the boot log |

### The real limit here is PSRAM bandwidth

Whatever else is tuned, this is the number that decides how the dash behaves.
The panel reads its frame buffer out of PSRAM continuously, and this board
measures only ~35-44 MB/s in total (19 MB/s of it left for the CPU while the
panel is scanning):

| pclk | refresh | panel needs |
|---|---|---|
| 24 MHz | 49 Hz | 37.9 MB/s - starves everything else |
| 20 MHz | 41 Hz | 31.6 MB/s |
| **16 MHz** | **33 Hz** | **25.3 MB/s - what this build uses** |
| 12 MHz | 24.6 Hz | 19.0 MB/s |

Two things make it worse: WiFi's buffers are allocated in PSRAM
(`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`, baked into the prebuilt IDF
libraries), and the dashboard's own polling is enough to cost most of the
frame rate - pausing it takes the render from 16 fps to 42 fps. Symptoms are
easy to misread: with a starved DMA the whole image rolls, which looks like a
badly tuned television rather than a software bug.

### Why two frame buffers, and why DIRECT mode

LVGL renders into one full-screen PSRAM buffer while the RGB peripheral scans
the other, and `esp_lcd_panel_draw_bitmap()` only returns once the swap has
happened at a frame boundary — so a frame is never half old and half new.

The render mode that pairs with it is **DIRECT** (a full-screen buffer, but
only the invalidated rectangles are drawn). `LV_DISPLAY_RENDER_MODE_FULL`
looks tempting but forces a whole 800x480 render *and* the same 768 KB of
PSRAM traffic every frame, which both wastes the CPU and starves the panel.

The panel then reads ~41 MB/s out of PSRAM continuously (800x480x16 bpp at
49 Hz) while the CPU needs the same memory for LVGL and WiFi. When the DMA's
prefetch falls behind, the peripheral loses sync with the DMA and the whole
picture shifts; Espressif ships `esp_lcd_rgb_panel_restart()` for it and
`CONFIG_LCD_RGB_RESTART_IN_VSYNC` is enabled in this build, which turns a
permanent shift into a jitter. `LCD_BOUNCE_LINES` is the margin against that:
the driver refills an internal-SRAM bounce buffer from PSRAM in bursts, and
the deeper it is the longer a WiFi burst can go on without emptying it. Two
of them are allocated, so 16 lines costs 2 x 25 600 B.

The pixel clock is taken from **PLL_D2 (240 MHz)**, which divides to 24 MHz by
an exact 10. On `PLL_F160M` the same 24 MHz needs the fractional divider
6 + 2/3; the exact divider removes any clock jitter from the picture. The
source is printed at boot, and `p` on the serial console steps the clock down
24 -> 20 -> 16 -> 12 MHz at runtime to test the bandwidth theory live.

### Serial console

115200 baud on UART0 (the CH340). Besides the boot banner:

| Key | Effect |
|---|---|
| `1`..`5` | toggle redraw groups: rev bar, tick numbers, big number, tile readouts, status bar |
| `0` / `9` | all on / all off |
| `r` | try the next GT911 axis mapping (see the touch note below) |
| `t` | reset the touch probe |
| `p` | step the pixel clock down one notch |
| `d` | print the frame buffer the panel is scanning as an ASCII luminance map |
| `n` | pause / resume the HTTP polling |
| `b` | toggle the bounce buffer between 0 and 16 lines |
| `e` | ESP-NOW vs HTTP, every field side by side |
| `E` | enable / disable the ESP-NOW listener (A/B its cost on one boot) |
| `h` | the list above |

`d` is the bench substitute for a camera: `tools/serial_cmd.py d 34` shows
what is on the glass, which is how the layout was verified without a screen
in front of you — and how the stale change was proved: dumping the frame
while live and while stale leaves all 36 rows identical except the two
status-bar rows, and within those only the dot and the state word.
`tools/serial_seq.py "35:d,42:n,48:d,56:n"` drives a whole timed scene in one
run (here: dump live, pause polling, dump stale, resume).

To stage a **stale** screen now that ESP-NOW carries the data, both transports
have to be stopped: `E` (stop listening) and then `n` (stop polling). Either
one alone leaves the dashboard live — which is the point of the fallback, and
`[stats] source ...` says which one is carrying it.

Geometry (bar cell size, the big number's row, tile widths and padding) is
derived in `include/theme.h`, so it is adjusted in one place. Only the top
band is a redesign — see `docs/redesign-bars.md`; mock-ups and the tool that
draws them are in `docs/mockups/` and `tools/mockup_dash.py`.

---

## 9. ESP-NOW

The C6 broadcasts a packed `OhxStatus` struct to the broadcast address every
100 ms (`OHX_STATUS_MS`) with **no web stack involved** — no access point
association, no sockets, no HTTP. This firmware listens to it, and since phase
2 it is **the source of the numbers on the glass**; HTTP stays for coolant
temperature, for the three lamps the packet has no matching flag for, and as
the fallback if the packets stop (§3).

| | |
|---|---|
| Wire format | `include/ohx_proto.h`, a **verbatim copy** of the C6's shared header (`b54d9a1d`, sha256 `9f1a6351...`). Refresh it from upstream; never edit it. |
| Listener | `src/net/ohx_now.{h,cpp}` — init, receive callback, decode, counters |
| Source choice | `src/net/datafeed.{h,cpp}` — ESP-NOW while packets arrive, HTTP otherwise |
| Build switch | `-DOHX_USE_ESP_NOW=1`; `0` compiles a build that only ever uses HTTP (the listener is not even initialised) |
| Console | `e` = every field, ESP-NOW beside HTTP. `E` = enable/disable the listener at runtime |
| Stats lines | `[stats] source esp-now (packet age 27 ms, http up)` and `[stats] esp-now 418 pkt (9.7 Hz), rej 0 dup 0 gap 3, ch 1, rssi -33 dBm` |
| Every change | `[feed] values now come from esp-now` / `... from http` |

Verified on the bench: 84-byte packets, magic/type/proto matching, decoded
field by field against the raw hexdump, 9.7-10.6 Hz sustained, 0 rejects and
~0.5 % sequence gaps (broadcast has no retransmission, so that is expected).
Listening costs **nothing** in render rate (16.2-16.4 fps with the `E` toggle
either way, and the loop is ~60.2 ms busy out of a 61.0 ms period).

Because ESP-NOW needs no association, the dashboard goes **LIVE before the
WiFi join completes**: `[ui] LIVE (data age 21 ms, source esp-now)` while
`http down`.

In the car, `e` is the cross-check that matters: it prints both transports
field by field, so any field whose scaling differs — `boost` is the one to
watch, since the packet documents it as "mbar above ambient" — is visible
rather than guessed at.

Full plan, risks and the phase write-ups:
[`docs/esp-now-plan.md`](docs/esp-now-plan.md).

---

## 10. Roadmap

The first item has a full plan in [`docs/esp-now-plan.md`](docs/esp-now-plan.md).


1. **ESP-NOW `OHX_STATUS` as the primary transport.** *Phases 1 and 2 done* —
   the dashboard is fed from the packet (§3, §9). Left: validate the value
   scaling in the car (`e`), then re-tune the pixel clock with the PSRAM the
   HTTP traffic was eating (16 -> 20 -> 24 MHz), and only then decide whether
   battery voltage, per-wheel slip or steering angle earn space on the layout.
   Phase 3 would add mode commands from the touchscreen (signed, off by
   default).
2. Battery voltage readout once ESP-NOW is in.
3. Config backup/restore over a small local web page served by the S3.

---

## 11. Layout of this repo

```
platformio.ini              build: ESP32-S3, OPI PSRAM, 4 MB OTA layout
partitions_4mb_ota.csv      two app slots + LittleFS + coredump

include/
  board_config.h            panel pins, timing, I2C, expander, touch
  theme.h                   palette, fonts, all layout constants
  lv_conf.h                 LVGL v9 config (partial; defaults fill the rest)
  ohx_proto.h               ESP-NOW wire format - verbatim copy of the C6's
                            shared header; refresh from upstream, never edit
  wifi_secrets.h.example    optional bench credentials

src/
  main.cpp                  boot banner, loop: ui tick + lv_timer_handler
  hal/
    crowpanel.h/.cpp        RGB panel, LVGL display+indev, backlight, GPIO38
    pca9557.h/.cpp          expander driver + I2C probe
  net/
    telemetry.h/.cpp        WiFi + /api/live + /api/dashboard, core 0 task
    ohx_now.h/.cpp          ESP-NOW listener: decode + counters
    datafeed.h/.cpp         source selection: ESP-NOW packets vs HTTP
  sys/
    config.h/.cpp           NVS-backed settings, validation, revision counter
  ui/
    dash_ui.h/.cpp          the whole dashboard in LVGL
    fonts/
      font_rpm_96.c         96 px rev digits, generated (tools/make_fonts.sh)
      font_num_56.c         56 px boost value, generated

tools/
  make_fonts.sh             regenerate src/ui/fonts from tools/fonts/*.ttf
  mockup_dash.py            draw the layout at panel size before coding it
  serial_capture.py         reset + read the console (read-only)
  serial_watch.py           read the console without resetting the board
  serial_shell.py           hold the port open: log + send keys, no reset
  serial_cmd.py             send one console command
  serial_seq.py             drive a timed sequence of console commands
  serial_tune.py            mask sweep: render cost per redraw group
  fonts/                    Montserrat Bold (OFL) + licence

docs/
  redesign-bars.md          why the ring went and what replaced it
  mockups/                  the mock-ups the layout was chosen from

.backup/                    factory 4 MB image (gitignored)
```

---

## 12. Verification status

| Item | Status |
|---|---|
| Board identified read-only | **done** — esptool + factory image strings |
| Panel pin map confirmed against the board | **done** — the factory image contains the exact 16-byte pin array at `0x1f0240` (see §8) |
| Factory firmware backed up | **done** — 4 MB, sha256 recorded (§5) |
| Firmware builds clean | **done** — see numbers below |
| Panel / touch bring-up | **done** — RGB panel, GT911 and the PCA9557 all come up |
| Link to a live C6 | **done** — `/api/live` and `/api/dashboard` at 1 Hz, plus the C6's ESP-NOW broadcast at ~10 Hz (§3) |
| Tearing / jitter fixed | **done** — visually confirmed stable. Two frame buffers + DIRECT mode, push only on `lv_display_flush_is_last()`, flush_ready only after the panel's frame callback, PLL_D2 clock, 24-line bounce buffer, pclk 16 MHz |
| Live data on screen | **done** — the fast fields come over ESP-NOW at ~10 Hz (§9); mode changes made on the OpenHaldex unit show up on the dash |
| Stale behaviour | **done** — the data stays on the glass and only the status light changes; proved by diffing frame dumps (§8) |
| ESP-NOW receive (phase 1) | **done** — 84-byte `OHX_STATUS` at ~9.7-10.6 Hz, 0 rejects, layout verified against the raw hexdump, costs no render rate |
| ESP-NOW drives the dashboard (phase 2) | **done** — `rpm`/`speed`/`throttle`/`boost`/`lock*`/`oil` and the brake, handbrake, coupling, temp-protection and CAN flags come from the packet; coolant and the TC/ASR/AWD lamps stay on HTTP; the HTTP fallback and the both-gone stale path were both staged on the bench (§3) |
| Touch input | **done** — the GT911's axes are mirrored by the library's default (`ROTATION_NORMAL` returns `width-x, height-y`): every press landed 180 degrees from the finger, so no button ever fired while the long press still worked (it is on the whole top band). Fixed with `ROTATION_INVERTED`; a tap on SETTINGS now reads ~(741, 465), which is where it is drawn |
| Settings page scroll | **done** — the card was repainting 780x420 px per step *plus* everything under it, and the dashboard kept invalidating 30x a second behind it: 67-84 ms per frame, 11-14 fps. The dashboard is now hidden while the page is up, the backdrop is opaque and `ui::tick()` stands still, which took a drag to ~9 ms per frame |
| Bar redesign (ring -> one bar) | **done** — the tach ring was ~34 ms and the needle's dirty patch ~23.5 ms of a 58.8 ms frame; the mask sweep is now flat across every redraw group at the unchanged 16 MHz pixel clock (`docs/redesign-bars.md`) |
| On-car test | not yet |

Build result:

```
RAM:   153 492 / 327 680 B   (46.8%)   <- 96 KB of that is LVGL's own heap
Flash: 1 618 742 / 1 966 080 B (82.3%) <- of the 1.875 MB OTA slot
```

(The bar redesign made flash go *down*: the deleted arc, needle, hub and tick
code outweighed the generated 96 px / 56 px fonts.)

### Memory, exactly

The ESP32-S3 has **512 KB of SRAM**. Only 327 680 B (320 KB) of it is
addressable as the application's DRAM region — the rest is IRAM and
ROM-reserved — so `46.6% of 327 680` is the normal way PlatformIO reports it
for this chip, not a sign that something is missing.

The 8 MB of **PSRAM** is the part that matters here: the two 800x480x16 bpp
frame buffers are 1 536 000 B, and they must live there. The build confirms
the `qio_opi` variant is linked (`CONFIG_SPIRAM_MODE_OCT 1`).

### One caveat worth knowing

The precompiled Arduino/IDF libraries in this platform were built assuming
`CONFIG_ESPTOOLPY_FLASHSIZE="16MB"`. This board has 4 MB. That is harmless
in practice — every partition is inside the first 4 MB, `board_upload.flash_size`
writes the correct size into the app image header, and esptool detects the
real chip — but if a future change ever needs libs compiled for exactly
4 MB, it requires `custom_sdkconfig` *plus* a project-local `core_dir`
(the OpenHaldex-C6 project does this, because rebuilding the libs writes
into the shared packages folder).

Nothing has been flashed. Flashing needs an explicit go-ahead.
