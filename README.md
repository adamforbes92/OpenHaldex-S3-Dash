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

The layout is a native port of `dash.html`, using the same geometry formula:

```
u = min(width / 100, height / 56)          -> 8 px at 800x480
```

```
+------------------------------------------------------------------+
|                                                                  |
|        .-''''''''''''''''''''-.            [SHIFT]                |
|      .'   3  .  4  .  5  .  6  '.                                 |
|     /  2                         7  \      tach ring:             |
|    |                                 |       green  0 .. rpmAmber |
|    |   2          6200         8     |       amber  .. rpmRedline |
|     \  1        RPM      .-.      0 /        red    .. rpmMax     |
|      '.     0          ( )        .'         needle + digits      |
|        '-._____________.-._______'                                |
+----------+----------+-------------+------------------------------+
|  SPEED   |  BOOST   | TEMPERATURES|  HALDEX                      |
|   123    |  1.25    | COOLANT  92 |  EXPERT                      |
|          |  bar     | [========]  |  coupling closed             |
|          | [======] | OIL     108 |  LOCK REQ   40%              |
|          | peak 1.8 | [=======]   |  LOCK ACT   38%              |
| peak 187 |          |             |  THR [===]                   |
+----------+----------+-------------+------------------------------+
| * LIVE   BRAKE HAND TC ASR AWD        RESET PEAKS  DAY  SETTINGS |
+------------------------------------------------------------------+
```

Everything that cannot be measured shows `--`, never a fake zero: a JSON
`null` from the C6 is carried all the way to the screen as "unknown".

Features carried over from the page: peak tracking, shift-light flash,
stale/disconnect overlay, day/night brightness, and a settings page with all
the same thresholds.

**Deliberate deviations from `dash.html`:**

* Status bar is **30 px** instead of 2.8u = 22 px, so the buttons are a
  usable touch target. The tach band absorbs the difference.
* Long-press anywhere on the tach opens settings, because the status-bar
  buttons are small for use in a moving car.
* Boost / speed / rpm are drawn with the "eased" value (110 ms time
  constant, same as the page) rather than snapping.

---

## 3. Data flow

```
        core 0                                core 1
+------------------------+          +--------------------------+
| WiFi STA -> OpenHaldex |          | loop():                  |
|                        |          |   ui::tick()   ~30 Hz    |
| GET /api/live   10 Hz  |  struct  |   lv_timer_handler()     |
| GET /api/dashboard 1 Hz| -------> |--------------------------|
+------------------------+  (mutex) | LVGL -> 2x 800x480 PSRAM |
                                   | frame buffers -> RGB DMA |
                                   +--------------------------+
```

| Endpoint | Rate | Fields used |
|---|---|---|
| `/api/live` | 100 ms | `rpm`, `speed`, `throttle`, `boost`, `coolantTemp`, `oilTemp`, `lockTarget`, `lockActual` |
| `/api/dashboard` | 1000 ms | `mode`, `haldexState`, `lockTarget`, `lockActual`, `chassisCAN`, `haldexCAN`, `brakeIn`, `handbrakeIn`/`handbrakeOut`, `tcOn`, `asrOn`, `awdWarning`, `couplingOpen`, `tempProtection` |

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
| poll live / dash / stale | 100 / 1000 / 500 ms |

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
  ESP-NOW broadcast (`supplyMv`), not over HTTP. A future version can add it
  by also listening on ESP-NOW (see §9).
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
| `1`..`6` | toggle redraw groups: arcs, needle, ticks, digits, readouts, status bar |
| `0` / `9` | all on / all off |
| `p` | step the pixel clock down one notch |
| `d` | print the frame buffer the panel is scanning as an ASCII luminance map |

`d` is the bench substitute for a camera: `tools/serial_cmd.py d 34` shows
what is on the glass, which is how the stale overlay and the layout were
verified without a screen in front of you.

Geometry (ring radius, tick spacing, tile widths) is all derived in
`include/theme.h` from the same formulas as `dash.html`, so it is adjusted
in one place.

---

## 9. Roadmap

The first item has a full plan in [`docs/esp-now-plan.md`](docs/esp-now-plan.md).


1. **ESP-NOW `OHX_STATUS` as the primary transport.** The C6 already
   broadcasts a richer packet every 100 ms (`OHX_STATUS_MS`) including
   battery voltage, clutch temperature, module temperature and the Haldex
   slip array — with no web stack, no sockets and no HTTP parsing. HTTP
   would stay only for the engine temps, which ESP-NOW does not carry.
2. Battery voltage readout once ESP-NOW is in.
3. Config backup/restore over a small local web page served by the S3.

---

## 10. Layout of this repo

```
platformio.ini              build: ESP32-S3, OPI PSRAM, 4 MB OTA layout
partitions_4mb_ota.csv      two app slots + LittleFS + coredump

include/
  board_config.h            panel pins, timing, I2C, expander, touch
  theme.h                   palette, fonts, all layout constants
  lv_conf.h                 LVGL v9 config (partial; defaults fill the rest)
  wifi_secrets.h.example    optional bench credentials

src/
  main.cpp                  boot banner, loop: ui tick + lv_timer_handler
  hal/
    crowpanel.h/.cpp        RGB panel, LVGL display+indev, backlight, GPIO38
    pca9557.h/.cpp          expander driver + I2C probe
  net/
    telemetry.h/.cpp        WiFi + /api/live + /api/dashboard, core 0 task
  sys/
    config.h/.cpp           NVS-backed settings, validation, revision counter
  ui/
    dash_ui.h/.cpp          the whole dashboard in LVGL

.backup/                    factory 4 MB image (gitignored)
```

---

## 11. Verification status

| Item | Status |
|---|---|
| Board identified read-only | **done** — esptool + factory image strings |
| Panel pin map confirmed against the board | **done** — the factory image contains the exact 16-byte pin array at `0x1f0240` (see §8) |
| Factory firmware backed up | **done** — 4 MB, sha256 recorded (§5) |
| Firmware builds clean | **done** — see numbers below |
| Panel / touch bring-up | **done** — RGB panel, GT911 and the PCA9557 all come up |
| Link to a live C6 | **done** — `/api/live` at 10 Hz and `/api/dashboard` at 1 Hz, `live 57 ok / 2 fail` over 30 s |
| Tearing / jitter fixed | **done** — visually confirmed stable. Two frame buffers + DIRECT mode, push only on `lv_display_flush_is_last()`, flush_ready only after the panel's frame callback, PLL_D2 clock, 24-line bounce buffer, pclk 16 MHz |
| Live data on screen | **done** — `/api/live` at 10 Hz; mode changes made on the OpenHaldex unit show up on the dash |
| On-car test | not yet |

Build result:

```
RAM:   152 684 / 327 680 B   (46.6%)   <- 96 KB of that is LVGL's own heap
Flash: 1 616 438 / 1 966 080 B (82.2%) <- of the 1.875 MB OTA slot
```

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
