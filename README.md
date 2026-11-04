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
| Flicker | lower `LCD_PCLK_HZ` (24 MHz -> 16 MHz) |
| Panel blank, no backlight | `PCA9557` not found — check the I2C scan line in the boot log |

Geometry (ring radius, tick spacing, tile widths) is all derived in
`include/theme.h` from the same formulas as `dash.html`, so it is adjusted
in one place.

---

## 9. Roadmap

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
| Panel / touch bring-up | **not yet run — requires flashing** |
| Link to a live C6 | **not yet run** |
| On-car test | not yet |

Build result:

```
RAM:   152 556 / 327 680 B   (46.6%)   <- 96 KB of that is LVGL's own heap
Flash: 1 602 762 / 1 966 080 B (81.5%) <- of the 1.875 MB OTA slot
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
