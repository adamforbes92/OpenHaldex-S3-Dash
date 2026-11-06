# OpenHaldex S3 Dash

A native dashboard for the **[OpenHaldex](#credits)** Haldex AWD controller,
running directly on a **7" ESP32-S3 touch panel**. It replaces the browser
dashboard — `dash.html`, served by the controller itself — with standalone
firmware, so the display boots straight to the gauge in the car. No phone, no
tablet, no browser, no WiFi client to keep awake.

The controller firmware is **not modified**. This project only reads what the
controller already publishes, so it can keep taking upstream updates.

> ### ⚠️ 100% AI-generated project
>
> Every line of this repository — firmware, build scripts, mock-ups and
> documentation — was written by **goose**, an open-source AI coding agent.
> Nothing here was typed by hand. The human role was direction, design
> approval, hardware bring-up and testing in the car.
>
> The models behind it: **`deepseek-v4.1-flash`** did the heavy lifting, with
> **`gpt-oss-20b`** running under **local inference** on the simpler tasks.
> `goose` orchestrates them and does the work on the wire.
>
> Treat it accordingly: it was built against one specific board (a CrowPanel
> 7.0" ESP32-S3), it is not security-audited, and the API it consumes is a
> hobby project's own. Read the code before you flash it.

---

## Contents

| | | |
|---|---|---|
| [Credits](#credits) | [6. Updating it in the car](#6-updating-it-in-the-car-over-the-air) | [11. ESP-NOW](#11-esp-now) |
| [1. What it shows](#1-what-it-shows) | [7. Configuration](#7-configuration) | [12. What this device cannot show](#12-what-this-device-cannot-show) |
| [2. Hardware](#2-hardware) | [8. Display tuning](#8-display-tuning) | [13. Repository layout](#13-repository-layout) |
| [3. Data flow](#3-data-flow) | [9. Serial console](#9-serial-console) | [14. Status](#14-status) |
| [4. Build](#4-build) | [10. What the car can fill](#10-what-the-car-can-fill) | [Licence](#licence) |
| [5. Before you flash: back up the board](#5-before-you-flash-back-up-the-board) | | [What is not built yet](TODO.md) |

---

## Credits

**[OpenHaldex](https://github.com/Forbes-Automotive/OpenHaldex-C6)** is an
open-source Haldex AWD controller for Volkswagen and Audi Group Gen1, Gen2 and
Gen4 differentials, built by **[Forbes Automotive](https://forbes-automotive.com)**.

Everything this dashboard displays originates there. The CAN decoding, the
Haldex control logic, the HTTP API and the ESP-NOW broadcast are all the
OpenHaldex-C6's work — this repository is only a **display client** for it.

If you are looking for the controller rather than a display for it, that is
the project you want.

The two sides communicate using interfaces the C6 already exposed, so neither
needed changing: `include/ohx_proto.h` here is a **verbatim copy** of the
controller's shared wire-format header (taken from `b54d9a1d`). Refresh it
from upstream; never edit it.

---

## 1. What it shows

![The dashboard layout](docs/mockups/g-fullwidth.png)

*Mock-up at the panel's real 800x480 geometry — `tools/mockup_dash.py` draws it.*

| Band | Contents |
|---|---|
| Rev bar | One segmented bar across the full 800 px, `1`–`7` above it and `x1000` at the right, then the rpm as a large centred number on its own row |
| Tiles | `SPEED` (km/h + peak) · `BOOST` (value, unit, a bar, peak) · `TEMPERATURES` (coolant and oil, each with a bar) · `HALDEX` (mode, coupling state, lock requested / actual, throttle) — **tap it to step the Haldex mode** |
| Status bar | Data-state dot, the BRAKE / HAND / TC / ASR / AWD lamps, rpm peak, and the RESET PEAKS / DAY / SETTINGS buttons |

The tile band follows the same geometry as the browser page, from one unit:

```
u = min(width / 100, height / 56)         ->  8 px at 800x480
```

REVS is the only full-width item; everything else is a tile, so no value
appears twice.

**Unmeasurable values show `--`, never a fake zero.** A JSON `null` from the
controller is carried all the way to the screen as "unknown", with a dash
(`NaN`) as the sentinel. Only the dash glyph draws `--`; a genuine `0` draws
`0`.

**Staleness moves the status light, not the data.** When live data stops
arriving the dashboard keeps the last values it had — bars stop where they
were, tiles keep their numbers — and the status dot turns amber with `STALE`.
Before the first sample ever arrives it reads `WAIT`, and `NO WIFI` in red if
the link itself is down. Nothing is blanked and no overlay covers the screen.

**Design choices specific to this display:**

* The status bar is **30 px** rather than the page's `2.8u` (22 px), so the
  buttons are a usable touch target. The band above absorbs the difference.
* **Long-press anywhere on the top band opens Settings**, because the
  status-bar buttons are small to hit in a moving car.
* Boost, speed and rpm are drawn with the *eased* value — a 110 ms time
  constant, matching the page — so they sweep rather than snap.
* Settings holds every threshold, the units, the brightness and the
  connection, and is stored in NVS so it survives a power cycle.

---

## 2. Hardware

| | |
|---|---|
| Board | Elecrow **CrowPanel 7.0"** HMI, `ESP32 Display-7.0` |
| MCU | ESP32-S3 QFN56 rev v0.2, dual core @ 240 MHz |
| PSRAM | 8 MB **OPI** (octal) — mandatory, see §5 |
| Flash | 4 MB quad |
| Panel | 800x480 RGB565 parallel (16 data lines + DE/VSYNC/HSYNC/PCLK) |
| Touch | GT911 capacitive, I2C on SDA 19 / SCL 20 at 400 kHz |
| Expander | PCA9557 I2C: IO0 = panel reset, IO1 = input |
| Backlight | GPIO 2 |
| USB | CH340 UART bridge (`/dev/ttyUSB0`), 460800 baud ceiling |

Nothing about the firmware is board-specific beyond `include/board_config.h`,
which holds the pin map, panel timings, I2C pins and the expander.

### The 5.0" variant

The CrowPanel **5.0"** uses the *same* 16 GPIOs but wired to the panel's RGB
inputs in a **different order**, and swaps DE/VSYNC. Building for it is one
flag:

```ini
build_flags = -DDASH_USE_50_PINMAP=1
```

A wrong pin map shows up immediately as a garbled or colour-swapped image. It
damages nothing.

### Touch axes

The GT911 library's `ROTATION_NORMAL` returns `(width - x, height - y)` — it
mirrors *both* axes — which puts every press 180° from the finger. This build
sets `ROTATION_INVERTED`, and the console's `r` key cycles the four mappings
if a differently-mounted panel needs another one.

---

## 3. Data flow

Two transports feed the dashboard. **ESP-NOW is the primary** — the
controller broadcasts a packed status packet every 100 ms with no web stack
involved, so there is no association, no socket and no TCP in that path. HTTP
is the fallback, and it still carries the fields the packet does not have.

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

`src/net/datafeed.{h,cpp}` is the single place that decides provenance:

| Field | Source |
|---|---|
| `rpm`, `speed`, `throttle`, `boost`, `lockTarget`, `lockActual` | ESP-NOW while packets arrive (holds 500 ms), else HTTP |
| `mode`, `haldexState`, `chassisCAN`, `haldexCAN`, `brakeIn`, `handbrakeIn`, `couplingOpen`, `tempProtection` | ESP-NOW — the packet has real bits for all of these |
| `coolant`, `oil` | always HTTP — the packet has no coolant field, and its `oilTemp` is the Haldex's oil, not the engine's |
| `tcOn`, `asrOn`, `awdWarning` | always HTTP — the packet carries `TC_FORCE` / `ASR_OFF` (off flags), which are not the same thing |
| packets stopped | everything falls back to HTTP, and says so on the console |

A numeric field the packet marks as "no data" falls back to the HTTP value if
HTTP has one, so a field this Haldex generation never fills in cannot make the
dashboard worse than it was. The source in use is printed in the stats line
and on every change (`[feed] values now come from esp-now`), and `e` on the
console dumps both transports side by side.

| Endpoint | Rate | Fields used |
|---|---|---|
| `/api/live` | 1000 ms | `coolantTemp` — and everything, if the packets stop |
| `/api/dashboard` | 1000 ms | `tcOn`, `asrOn`, `awdWarning` — and everything, if the packets stop |

Because ESP-NOW needs no association, the dashboard goes **LIVE before the
WiFi join completes** — `[ui] LIVE (data age 21 ms, source esp-now)` while
`http down`.

### Why HTTP is still there

Three reasons, all deliberate: coolant temperature is not in the packet; the
three lamps above are not in it with matching semantics; and if the controller
stops broadcasting (older firmware, ESP-NOW disabled, out of range) the
dashboard keeps working at 1 Hz instead of going dark.

### Why two poll rates

`/api/dashboard` on the controller calls `getCPUUsagePercent()`, which mallocs
and walks every task on the controller's **single** core. Polling that faster
buys no extra visual resolution. Splitting fast and slow data keeps the same
look for roughly 90 % less CPU on the controller.

`/api/dashboard` is also never polled slower than **2 s**: it is the only
handler that calls `otaNoteWebActivity()`, and the controller drops its access
point after about five minutes without it. The settings page clamps
`pollDashMs` to 250..2000 ms for exactly this reason.

### Joining the controller's access point

The OpenHaldex-C6 publishes an **open** access point by default, which two
defaults in the Arduino core have to be worked around:

* `WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK)` is the core's default, so an open
  network is *below* the threshold. The station is therefore told
  `WIFI_AUTH_OPEN` before `WiFi.begin()`.
* Association and DHCP are separate problems. The board associates fine and
  then never gets a lease, which looks identical to "AP is dead" from outside.
  The cause is that the controller is usually *also* on a home LAN already
  using `192.168.1.0/24` — its softAP and station interfaces then share a
  subnet, and its DHCP server cannot hand out an address that looks free. The
  firmware logs `associated but no DHCP lease` and retries with a static
  address derived from the configured host (`192.168.1.203`), skipping DHCP
  entirely. That suits a fixed install: the dash talks to one AP at one
  address.

The join loop prints BSSID, channel and RSSI, which only read back once the
station is associated, so the two failure modes can be told apart.

### Connection reuse

Both pollers use `HTTPClient::setReuse(true)`, so the two requests per second
share one TCP connection instead of opening two. That matters: the controller
runs with `CONFIG_LWIP_MAX_SOCKETS=8`, and a handshake-per-request pattern
would fill its socket table with `TIME_WAIT` entries.

---

## 4. Build

```bash
cd OpenHaldex-S3-Dash
./.venv/bin/pio run -e s3-dash
```

The plan is a plain [PlatformIO](https://platformio.org) project. The first
build downloads the pioarduino `espressif32@55.3.312` platform — the same
release the OpenHaldex-C6 firmware uses, so both projects build against an
identical toolchain — plus the framework, which takes a few minutes, once.

**If the platform disappears on the first build**, the installed PlatformIO
core is reporting itself as a post-release of 6.2.0 (`6.2.0.post1`), which
PlatformIO turns into the semver *pre-release* `6.2.0-post.1`. The platform
declares `engines >= 6.2.0`, the pre-release fails that test, and Platform
Manager deletes the platform — leaving `~/.platformio/platforms` empty and a
build log that ends with `IncompatiblePlatform`. The core is a post-release,
not a pre-release, so report it as plain 6.2.0: in `platformio/__init__.py`,
set

```python
VERSION = (6, 2, 0)
```

**PSRAM must be OPI.** The panel driver needs two 800x480x16 bpp frame buffers
(1.50 MB) reachable by the LCD DMA engine. `platformio.ini` sets
`board_build.arduino.memory_type = qio_opi`; without it `lcd.begin()` fails
and nothing is drawn.

### Flash

```bash
# with the board in download mode:
./.venv/bin/pio run -e s3-dash -t upload
```

`upload_speed` is pinned to **460800**. The CH340 on this board fails at
921600 with `Invalid SLIP escape (0xdb, 0xBD)`.

---

## 5. Before you flash: back up the board

The CrowPanel ships with a factory application. Reading the flash is
read-only and risk-free, and it is worth having:

```bash
./.venv/bin/esptool -p /dev/ttyUSB0 -b 460800 read_flash 0x0 0x400000 \
    .backup/s3_factory_full_4MB.bin
```

Restore the original image at any time with:

```bash
./.venv/bin/esptool -p /dev/ttyUSB0 -b 460800 write_flash 0x0 \
    .backup/s3_factory_full_4MB.bin
```

The board's factory partition table, as read from that image:

| Label | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 0x5000 |
| otadata | data/ota | 0xe000 | 0x2000 |
| app0 | app/ota_0 | 0x10000 | 0x3c0000 |
| spiffs | data/spiffs | 0x3d0000 | 0x20000 |
| coredump | data/coredump | 0x3f0000 | 0x10000 |

This firmware replaces it with `partitions_4mb_ota.csv`, which trades app size
for **two app slots (OTA)**, a 128 KB LittleFS partition and a coredump
partition:

| Label | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 0x5000 |
| otadata | data/ota | 0xe000 | 0x2000 |
| app0 | app/ota_0 | 0x10000 | 0x1e0000 |
| app1 | app/ota_1 | 0x1f0000 | 0x1e0000 |
| littlefs | data/spiffs | 0x3d0000 | 0x20000 |
| coredump | data/coredump | 0x3f0000 | 0x10000 |

OTA matters once the dash is mounted behind the dashboard, where there is no
USB cable. It is deliberately **after** the mode switch: a dash that can
change one thing on the car is worth more than one that can be reflashed, and
the flashing side of OTA needs no new partition to be useful.

---

## 6. Updating it in the car (over the air)

Once the dash is screwed into a pod there is no USB cable, so it can update
itself from a phone. The dash is **already** a station on the controller's
access point — that is how it gets its data — so this needs no new networking
at all:

1. Download `firmware.bin` from the
   [releases page](https://github.com/adruss85/OpenHaldex-S3-Dash/releases)
   onto the phone.
2. Join the controller's access point (`OpenHaldex-C6` by default) — the same
   thing you do to see the controller's own dashboard.
3. Open the address shown on the dash's SETTINGS page, under **UPDATE**:
   `http://192.168.1.203/` (or `http://ohx-dash.local/`).
4. Choose the file and press **Upload and reboot**. It takes about 15 s for
   the ~1.6 MB image, and the page reports the version, the slot and how many
   bytes went in.

Both addresses work because the dash takes a **static** address after the
first join: the controller's access point hands out no leases when it is also
sitting on a home LAN with the same subnet, which is a quirk of this setup
rather than a fault, and the dash learns its way around it.

### The way back, which is the part that matters

Anything written over the air is a **trial** until the dash proves it works.

```
old image  ->  records "the next boot is on trial" in NVS, then reboots
new image  ->  boots, brings the panel up, holds it for 20 s
           ->  stands the trial down: this image is now the confirmed one
```

If that does not happen, the dash puts the previous image back by itself:

| Give-up condition | Why |
|---|---|
| the panel never comes up within 90 s | the image runs, but the thing you look at does not |
| the trial image has already tried to boot 3 times | a crash loop |
| a trial told to fail, from the console (`O`) | how the path is tested without a bad build |

The trial lives in its own NVS namespace because the *outgoing* image is the
one that writes it. The image that is being replaced is never modified: a
power cut in the middle of a write leaves the running slot untouched and the
boot selection unchanged, so the dash comes back exactly as it was.

### What it checks, and what it does not

| Check | Enforced |
|---|---|
| the file is a real ESP image (magic, checksum) | yes, by the IDF |
| it is an image for **this chip** (ESP32-S3) | yes, by the IDF — a `.bin` for another chip is refused |
| it is an image for **this project** | **no** — a valid ESP32-S3 image that is not this dashboard would be accepted, boot, and never stand the trial down. Until that is closed, take the file from this repository's releases page and nothing else |

Nothing is fetched from the internet. The dash never checks for updates: it
has no idea a newer version exists, which is deliberate — nothing on this
device reaches out on its own.

### From the console

| Key | |
|---|---|
| `o` | where the update page is, which slot is running, whether it is on trial |
| `O` | bench test: arm the next boot as a *failing* trial and reboot, so the fall-back path is exercised for real. Refused unless the other slot actually holds an image |

---

## 7. Configuration

Defaults match the browser page:


| Setting | Default |
|---|---|
| SSID / password | `OpenHaldex-C6` / *(empty — the controller's AP is open by default)* |
| Host | `192.168.1.1` (the access-point address; `openhaldex.local` also works) |
| rpm max / amber / redline / shift | 8000 / 6000 / 6800 / 7300 |
| Coolant warn / crit (C) | 100 / 110 |
| Oil warn / crit (C) | 120 / 135 |
| Boost max / warn / crit (mbar gauge) | 2500 / 1800 / 2200 |
| Temperature / boost units | C / bar |
| Poll live / dash / stale | 1000 / 1000 / 1500 ms |

All of it is editable on the device's Settings page and stored in NVS
(`Preferences`, namespace `ohxdash`) as one versioned blob, so a firmware
update that changes the struct layout falls back to defaults instead of
reading garbage. Saving only rejoins the WiFi if the SSID, password, host or
port actually changed.

**No credentials are in git.** For bench work, copy
`include/wifi_secrets.h.example` to `include/wifi_secrets.h` (gitignored) and
fill it in. It is only a first-boot seed — the settings page always wins
afterwards.

### Units

Boost is **millibar gauge** (above ambient). The controller decodes both MQB
`Ladedruck` (0x107) and PQ (0x588) as `(raw * 10) - 1000` and clamps to
`>= 0`, so 1000 mbar ≈ 1.00 bar ≈ 14.5 psi.

---

## 8. Display tuning

The RGB panel configuration in `src/hal/crowpanel.cpp` is Elecrow's own 7.0"
description. Symptoms and where to look if a board is reworked or is a
different revision:

| Symptom | Fix |
|---|---|
| Colours swapped (red shows as blue) | build with `-DDASH_USE_50_PINMAP=1`, or permute `kDataPins` |
| Image shifted / wrapped | `LCD_HSYNC_*` / `LCD_VSYNC_*` porches in `board_config.h` |
| Whole image jitters / shifts | panel DMA starved of PSRAM bandwidth: raise `LCD_BOUNCE_LINES`, then lower `LCD_PCLK_HZ` |
| Image tears or rolls | LVGL is drawing into the buffer being scanned: keep `LCD_USE_DOUBLE_FB 1` and `LV_DISPLAY_RENDER_MODE_DIRECT` |
| Panel blank, no backlight | the PCA9557 was not found — check the I2C scan line in the boot log |

### The real limit is PSRAM bandwidth

Whatever else is tuned, this is the number that decides how the dash behaves.
The panel reads its frame buffer out of PSRAM continuously, and this board
measures only ~35–44 MB/s in total, with ~19 MB/s of that left for the CPU
while the panel is scanning:

| pclk | refresh | panel needs |
|---|---|---|
| 24 MHz | 49 Hz | 37.9 MB/s — starves everything else |
| 20 MHz | 41 Hz | 31.6 MB/s |
| **16 MHz** | **33 Hz** | **25.3 MB/s — what this build uses** |
| 12 MHz | 24.6 Hz | 19.0 MB/s |

Two things make it worse. WiFi's buffers are allocated in PSRAM
(`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`, baked into the prebuilt IDF
libraries), so network traffic competes with the panel directly — pausing the
HTTP polling alone took the render rate from 16 fps to 42 fps on the bench.

Symptoms are easy to misread: with a starved DMA the whole image rolls, which
looks like a badly tuned television rather than a software bug.

### Two frame buffers, and DIRECT mode

LVGL renders into one full-screen PSRAM buffer while the RGB peripheral scans
the other, and `esp_lcd_panel_draw_bitmap()` only returns once the swap has
happened at a frame boundary — so a frame is never half old and half new.

The render mode that pairs with it is **DIRECT**: a full-screen buffer, but
only the invalidated rectangles are drawn.
`LV_DISPLAY_RENDER_MODE_FULL` looks tempting but forces a whole 800x480 render
*and* a 768 KB PSRAM write every frame, which both wastes the CPU and starves
the panel.

When the DMA's prefetch falls behind, the peripheral loses sync with the DMA
and the picture shifts. Espressif ships `esp_lcd_rgb_panel_restart()` for it
and `CONFIG_LCD_RGB_RESTART_IN_VSYNC` is enabled in this build, which turns a
permanent shift into a jitter. `LCD_BOUNCE_LINES` is the margin against that:
the driver refills an internal-SRAM bounce buffer from PSRAM in bursts, and
the deeper it is, the longer a WiFi burst can run without emptying it. Two of
them are allocated, so 16 lines costs 2 x 25 600 B.

The pixel clock is taken from **PLL_D2 (240 MHz)**, which divides to 24 MHz by
an exact 10. On `PLL_F160M` the same 24 MHz needs the fractional divider
6 + 2/3; the exact divider removes any clock jitter from the picture. The
source is printed at boot, and `p` on the serial console steps the clock down
24 → 20 → 16 → 12 MHz at runtime to test the bandwidth theory live.

### Where the layout lives

Bar cell size, the big number's row, tile widths and padding are all derived
in `include/theme.h`, so geometry is adjusted in one place. The reasoning
behind the top band is in `docs/redesign-bars.md`, and the mock-up tool that
draws the layout at panel size before it is coded is `tools/mockup_dash.py`.

---

## 9. Serial console

115200 baud on UART0 (the CH340). Besides the boot banner:

| Key | Effect |
|---|---|
| `1`..`5` | toggle redraw groups: rev bar, tick numbers, big number, tile readouts, status bar |
| `0` / `9` | all redraw groups on / off |
| `p` | step the pixel clock down one notch |
| `d` | print the frame buffer the panel is scanning as an ASCII luminance map |
| `n` | pause / resume the HTTP polling |
| `e` | ESP-NOW vs HTTP, every field side by side |
| `f` | forget the remembered controller (see [Which controller](#which-controller)) |
| `x` | open / close the raw telemetry page (all of the above on the glass) |
| `v` | force the other tile band, for bench work (see 'What the car can fill') |
| `m` | step the Haldex mode — the same as tapping the HALDEX panel |
| `E` | enable / disable the ESP-NOW listener (A/B its cost on one boot) |
| `b` | toggle the bounce buffer between 0 and 16 lines |
| `r` | try the next GT911 axis mapping |
| `t` | reset the touch probe |
| `h` | the list above |

`d` is the bench substitute for a camera: `tools/serial_cmd.py d 34` prints
what is on the glass, which is how the layout was verified without a screen in
front of you. `tools/serial_seq.py "35:d,42:n,48:d,56:n"` drives a whole timed
scene in one run.

`x` (or **SETTINGS → RAW DATA**) opens the **raw telemetry page**: every value
the dash can see, on both transports, with a field the C6 marks as "no value"
drawn as `--` rather than `0`. It is the instrument for a road test — what the
packet carries versus what the JSON carries, side by side — so a problem can be
read off the panel or pasted from the console instead of being described.

The page prints itself to the console when it opens and whenever **FREEZE** is
pressed, which holds the last reading on the glass long enough to photograph.

To stage a **stale** screen, both transports have to be stopped — `E` (stop
listening) then `n` (stop polling). Either one alone leaves the dashboard live,
which is the point of the fallback; `[stats] source ...` says which one is
carrying it.

> **Opening this board's serial port reboots the ESP32.** That is measured,
> not assumed: with a 12 s gap between two runs, the second still begins with a
> full boot banner, so the reset comes from the kernel asserting DTR/RTS as the
> port is opened, before any userspace code can touch the lines. Nothing held in
> RAM survives it. `tools/serial_shell.py` therefore opens the port **once** and
> keeps it, so a whole session of gestures and console keys is not reset halfway
> through, and `tools/serial_capture.py` is the one to use when a reset is
> exactly what you want.

---

## 10. What the car can fill

The dashboard's shape is decided once at boot from what the controller says it
can feed, not fixed for every car. Two ways of finding out, and which was used
is shown on the raw page so it can be argued with:

| | How | Why |
|---|---|---|
| **detected** | the packet marks a field it has no value for with a sentinel, which arrives as NaN | the car is telling us, rather than us assuming |
| **table** | the Haldex generation | boost is the one field with no sentinel: a car with no boost frame reads a confident `0.00`, so it cannot be detected |

Measured on the Gen1 car this was built against, and logged as one line at
boot:

```
[cap] gen 1 | boost no (table) | supply no (detected) | haldex temps no
(detected) | steering yes (detected) | NoBoost (A) | 20 pkts
```

| Capability | This car | Consequence |
|---|---|---|
| boost | absent | the BOOST panel is not built into the band |
| supply voltage | absent | not shown |
| Haldex oil / clutch / module temps | absent | not shown |
| steering angle | **packet only** | the packet is authoritative; the JSON path gates it off for Gen1 |
| handbrake | **chassis CAN only** | the lamp lights from the CAN bit; the wired input never moves on this car |
| brake | wire, and CAN | the lamp lights from either |

Two tile bands exist, both approved as mock-ups in `docs/mockups/`:

* **NoBoost (A)** — SPEED | TEMPERATURES | HALDEX, three equal panels (266 / 266
  / 268), for a car with no boost source. What this car gets.
* **Full (B)** — SPEED | BOOST | TEMPERATURES | HALDEX, the widths `dash.html`
  gives them, for a car that has one. **Unverified**: no Gen4/5 car has been
  available to test it on.

The decision latches. It is not re-evaluated per frame, because a layout that
changed shape while the car was moving would be worse than a widget that reads
`--`, and an unsettled dashboard keeps the full band rather than hiding a widget
on a guess. `v` on the console forces either band, which is how the unverified
one gets looked at.

## 11. ESP-NOW

The controller broadcasts a packed `OhxStatus` struct to the broadcast address
every 100 ms with **no web stack involved** — no access-point association, no
sockets, no HTTP. This firmware listens to it and it carries the numbers on the
glass; HTTP stays for coolant temperature, for the three lamps the packet has
no matching flag for, and as the fallback if the packets stop.

| | |
|---|---|
| Wire format | `include/ohx_proto.h`, a **verbatim copy** of the controller's shared header (`b54d9a1d`). Refresh it from upstream; never edit it. |
| Listener | `src/net/ohx_now.{h,cpp}` — init, receive callback, decode, counters |
| Source choice | `src/net/datafeed.{h,cpp}` — ESP-NOW while packets arrive, HTTP otherwise |
| Build switch | `-DOHX_USE_ESP_NOW=1`; `0` compiles a build that only ever uses HTTP, and the listener is not initialised at all |
| Console | `e` = every field, ESP-NOW beside HTTP. `E` = enable/disable the listener at runtime |
| Stats | `[stats] source esp-now (packet age 27 ms, http up)` and `[stats] esp-now 418 pkt (9.7 Hz), rej 0 dup 0 gap 3, ch 1, rssi -33 dBm` |
| Every change | `[feed] values now come from esp-now` / `... from http` |

84-byte packets arrive at 9.7–10.6 Hz with 0 rejects and ~0.5 % sequence gaps
— broadcast has no retransmission, so gaps are expected. Listening costs
nothing in render rate: the loop stays at ~60.2 ms busy out of a 61.0 ms
period with the listener on or off.

### Which controller

A broadcast reaches every dash in range, and two controllers can share the
default SSID. So the first one heard whose name matches the configured SSID is
remembered by MAC, in NVS, and from then on:

* packets from anyone else are counted (`ignored`) and dropped, with one console
  line per MAC;
* commands are only ever sent to that MAC;
* the station's join is aimed at that MAC, so the HTTP values cannot come from a
  different car than the ESP-NOW ones.

A replaced controller is picked up by changing the SSID, pressing **DEFAULTS** in
Settings, or `f` on the console. The first boot takes whichever matching
controller it hears first, so do it away from other cars.

### In the car

`e` is the cross-check that matters: it prints both transports
field by field, so any field whose scaling differs — `boost` is the one to
watch, since the packet documents it as "mbar above ambient" — is visible
rather than guessed at.

### Sending: the mode switch

The same wire format carries the other way. **Tap anywhere on the HALDEX
panel** and the mode steps on: Stock → FWD → 50:50 → 60:40 → 75:25 → Expert →
Stock. A standalone controller has no Stock to go to (it answers by staying on
its last mode), so there the step goes from Expert back to FWD. One tap, one
step — no list, no confirmation, no modal. `m` on the
console does exactly the same thing, which is how the path gets exercised
without touching the glass.

The command is 22 bytes — magic, type, proto, the controller's nonce, a
per-dash sequence, the command byte and its arguments — signed with
**HMAC-SHA256**, truncated to 8 bytes. The key is the controller's
access-point password: the same string the station joins with. It is unicast
to the [remembered controller](#which-controller), and this is the only thing
this firmware ever transmits.

The controller does not reply directly. It publishes the answer in the ack
fields of its next broadcast (`ackSrc`, `ackSeq`, `ackResult`), so the
verdict arrives with the same 10 Hz stream as the data, and the panel's
sub-line reports both halves of the exchange:

```
[ui] mode -> FWD (sent)
[ui] mode FWD: ok (ack after 60 ms)
```

| On the panel | Meaning |
|---|---|
| `FWD ...` | sent, waiting for the controller |
| `FWD ok` | accepted and applied |
| `FWD control off` | ESP-NOW commands are switched off on the controller |
| `FWD wrong key` | the password here differs from the one on the controller |
| `FWD no link` | nothing was sent: no packet heard from the controller yet |
| `FWD no reply` | sent, but nothing came back within 1.5 s |

A `stale` result (another command — the web UI, a second gauge — used the
nonce first) earns **one** silent retry, because the nonce is refilled every
100 ms and a second race would just lose the same way.

**An open access point is a warning, not a gate.** With no password the HMAC
is keyed with an empty string, and the controller accepts commands signed that
way if it has commands enabled — this is the state the reference car was in
when the table above was captured. Settings says so in plain words (`signing
key: NONE` plus a warning line) and leaves the switch working, because whether
a command is obeyed is the controller's decision and the result code reports
it honestly. The nonce is broadcast, so on an open access point anyone in
range can compute a signature; the visible, immediate mode change on the panel
is what makes that noticeable.

Full plan, risks and phase notes: [`docs/esp-now-plan.md`](docs/esp-now-plan.md).

---

## 12. What this device cannot show

Data the controller does not expose, or has no room for here:

* Engine oil **pressure**, fuel pressure, individual wheel speeds.
* Gear position and clutch state.
* Battery voltage — this *is* available, but only in the ESP-NOW broadcast
  (`supplyMv`), not over HTTP. The dash receives it; it has no space on the
  layout yet.
* Anything at all while the chassis CAN is silent: on the bench with no
  vehicle, the controller reports `rpm`/`speed`/`throttle`/`boost` as JSON
  `null`, and this firmware correctly shows `--`.

---

## 13. Repository layout

```
platformio.ini              build: ESP32-S3, OPI PSRAM, 4 MB OTA layout
partitions_4mb_ota.csv      two app slots + LittleFS + coredump

include/
  board_config.h            panel pins, timing, I2C, expander, touch
  theme.h                   palette, fonts, all layout constants
  lv_conf.h                 LVGL v9 config (partial; defaults fill the rest)
  ohx_proto.h               ESP-NOW wire format - verbatim copy of the
                            controller's shared header; refresh, never edit
  wifi_secrets.h.example    optional bench credentials

src/
  main.cpp                  boot banner, loop: ui tick + lv_timer_handler
  hal/
    crowpanel.h/.cpp        RGB panel, LVGL display+indev, backlight, touch
    pca9557.h/.cpp          expander driver + I2C probe
  net/
    telemetry.h/.cpp        WiFi + /api/live + /api/dashboard, core 0 task
    ohx_now.h/.cpp          ESP-NOW listener: decode + counters
    datafeed.h/.cpp         source selection: ESP-NOW packets vs HTTP
  app/
    config.h/.cpp           NVS-backed settings, validation, revision counter
  ui/
    dash_ui.h/.cpp          the whole dashboard in LVGL
    raw_ui.h/.cpp           raw telemetry page: both transports, no
                            interpretation, "--" for a field with no value
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
  redesign-bars.md          why the top band is a bar, and its measurements
  esp-now-plan.md           the ESP-NOW transport, its risks and phases
  mockups/                  the mock-ups the layout was chosen from
```

`tools/make_fonts.sh` is only needed to *regenerate* the two big fonts. LVGL
ships Montserrat up to 48 px, so the 96 px rev digits and 56 px boost value are
generated from the TTF in `tools/fonts/`; the output is committed, so a normal
build needs neither node nor the script.

---

## 14. Status

Working on the bench against a live controller, and now verified in the car:

* Panel, touch and the PCA9557 expander all come up; tearing and jitter are
  dealt with (two frame buffers, DIRECT mode, push only on
  `lv_display_flush_is_last()`, PLL_D2 clock, 24-line bounce buffer, 16 MHz).
* Both transports are live: HTTP at 1 Hz and the ESP-NOW broadcast at ~10 Hz,
  with the source printed and switchable.
* Stale behaviour, the settings page, the touch mapping and all five redraw
  groups have been measured on the bench.
* **In the car**: the raw telemetry page renders and every field matches what
  the controller reports; the BRAKE and HAND lamps light from the source that
  actually moves on this car; the capability decision settles to the NoBoost
  band; and **the mode switch works** — a tap on the HALDEX panel, and `m` on
  the console, each got `ok` back from the controller in ~60 ms.
* **Over the air**: a real 1.7 MB upload from a browser wrote the other slot,
  rebooted into it *on trial*, and stood the trial down once the panel had
  held for 20 s. The fall-back was then exercised for real — a trial told to
  fail handed the car back to the previous slot in both directions
  (app0 → app1 → app0). A file that is not a firmware image is refused with
  HTTP 500 and nothing written. Confirmed from a phone on the controller's own
  access point as well.

**Still to be exercised in the car:** the value scaling for `boost` — none of
the reference car's data has come from a boost source yet, since a Gen1 Haldex
has no boost frame at all, and the Full band itself remains unverified. One
unexplained chip reset was seen 8 s after the first post-update boot while the
station was re-associating (no panic, clean ROM banner); 180 s of idle running
afterwards showed no repeats, and the boot banner now prints the reason so it
can be caught if it happens again.

Next: [TODO.md](TODO.md) — what is deliberately not built yet, and why.

Build result:

```
RAM:   155 028 / 327 680 B   (47.3%)   <- 96 KB of that is LVGL's own heap
Flash: 1 675 258 / 1 966 080 B (85.2%) <- of the 1.875 MB OTA slot
```

### Memory, exactly

The ESP32-S3 has **512 KB of SRAM**, but only 327 680 B (320 KB) of it is
addressable as the application's DRAM region — the rest is IRAM and
ROM-reserved — so `47.3% of 327 680` is the normal way PlatformIO reports it
for this chip, not a sign that something is missing.

The 8 MB of **PSRAM** is the part that matters here: the two 800x480x16 bpp
frame buffers are 1 536 000 B and they must live there. The build confirms the
`qio_opi` variant is linked (`CONFIG_SPIRAM_MODE_OCT 1`).

### One caveat worth knowing

The precompiled Arduino/IDF libraries in this platform were built assuming
`CONFIG_ESPTOOLPY_FLASHSIZE="16MB"`. This board has 4 MB. That is harmless in
practice — every partition is inside the first 4 MB, `board_upload.flash_size`
writes the correct size into the app image header, and esptool detects the real
chip — but if a future change ever needs libs compiled for exactly 4 MB, it
requires `custom_sdkconfig` *plus* a project-local `core_dir`, because
rebuilding the libs writes into the shared packages folder.

---

## Licence

This project is licensed under the **GNU General Public License v3.0 (GPL‑v3)**, which requires that any derivative work also be distributed under the same license. The full text is in the `LICENSE` file.

It depends on **[OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6)**
at runtime and copies its wire-format header verbatim — see that project for
its terms. The bundled Montserrat Bold font is under the SIL Open Font
Licence, reproduced in `tools/fonts/OFL.txt`. LVGL, the GT911 library,
ArduinoJson and the ESP-IDF are all used under their own licences.
