# The bar redesign

Replaces the top band of the dashboard — the tach ring, its needle and the
9 tick marks — with one full-width segmented bar and a large rev number on a
row of its own. The four original tiles and the status bar keep their
geometry; only the band above them changed.

## Why the ring had to go

Measured on the real panel with `tools/serial_tune.py`, at the 16 MHz pixel
clock, with the ring still in place (`[stats] render` every 5 s, mask toggles
on the console):

| state | fps | ms/loop | ms/busy | ms/flush |
|---|---|---|---|---|
| everything on | 16.4 | 58.8 | 60.3 | 23.6 |
| arcs off | 32.0 | 24.5 | 25.3 | 9.3 |
| arcs + needle off | 62.2 | 1.0 | 1.4 | 0.0 |
| ticks, numbers, readouts, status off | 63.0 | — | — | — |

So the ring was ~34 ms and the needle's dirty patch another ~23.5 ms of a
58.8 ms frame; the tiles, the numbers and the status bar were free (~1 ms).

The mechanism (hypothesis, not proved): `lv_arc` invalidates the arc area on
every angle change *and* calls `inv_knob_area()` twice, and the needle's
bounding box always overlaps the ring. LVGL has no occlusion cache, so
anything dirtied inside the ring dragged three anti-aliased arcs back
through a per-pixel-coverage redraw.

`LV_DRAW_SW_DRAW_UNIT_CNT 2` and dirty-area hygiene were both tried and
rejected as fixes; the ring was driving the frame budget, so it went.

## What replaced it

One bar across the whole panel, its number on a row of its own underneath, and
the four original tiles below that:

```
+------------------------------------------------------------------+
| [###############################################.............]  |  REVS bar
|  1    2    3    4    5    6    7                        x1000    |  full width
|                         6800                                     |  96 px row
+---------+----------+--------------+-----------------------------+
| SPEED   | BOOST    | TEMPERATURES | HALDEX                      |
+---------+----------+--------------+-----------------------------+
| * LIVE  BRAKE HAND TC ASR AWD  rpm peak  RESET PEAKS DAY SETTINGS|
+------------------------------------------------------------------+
```

- **40 opaque cells** (`BAR_SEG 40`, `BAR_GAP 2`, `BAR_W` = the full 800 px, so
  20 px per cell). A change of value moves the edge by one cell, so one
  rectangle is repainted instead of an arc.
- **Per-cell state is cached.** `g_revLit[]` holds the last lit/unlit state per
  cell with `-1` meaning "unknown" (forces a repaint after the build and after
  a settings change); `setBgColor()` also guards on the current value. There is
  no unconditional invalidate anywhere.
- **Zone colours are computed once**, in `syncGauge()`, from `rpmAmber` /
  `rpmRedline` — each cell carries its own lit and unlit colour, so the bar
  shows the shift and redline zones before the value reaches them.
- **The number gets its own row**, centred, in a label box exactly one line
  high: "centred" is arithmetic, not a layout pass, and it stays centred if the
  generated font changes size.
- **Only two digits change per frame** in the number, so `setSuffix()` repaints
  the label only when the text actually changes.
- **REVS is the only full-width item.** Everything else is a tile, so no value
  is shown twice: boost is a tile again (value, unit, a small white bar, its
  peak) and the warning colour lives on the value.
- **Peaks live where their value does** — SPEED and BOOST in their tiles. The
  rpm peak has no tile, so it stays in the status bar.

### Two generated fonts

LVGL ships Montserrat only up to 48 px, and the rev readout wants 96 px
(Montserrat Bold `6800` is 276 px at 104 px — too wide for the row it sits in
next to nothing, and 255 px at 96 px). `tools/make_fonts.sh` regenerates
`src/ui/fonts/font_rpm_96.c` and `font_num_56.c` from
`tools/fonts/Montserrat-Bold.ttf` with `lv_font_conv`; the output is committed,
so a normal build needs neither node nor the script.

Both fonts are RLE-compressed (`bitmap_format = 1`), which is why
`LV_USE_FONT_COMPRESSED 1` is set in `include/lv_conf.h`. Without it
`lv_font_get_glyph_bitmap()` returns NULL for every glyph and the letter
drawing code hard-faults — that is a crash, not a fallback. `--no-compress`
would avoid the requirement at 121 471 B instead of 42 728 B for the 96 px set.

## Result

Same 16 MHz pixel clock, same panel, `tools/serial_tune.py` again after the
change:

| state | fps | ms/loop | ms/busy | gauge ms/tick | vsync |
|---|---|---|---|---|---|
| everything on | 61.4 | 0.8 | 1.1 | 0.07 | 33/s |
| rev bar off | 62.4 | 1.0 | 1.4 | 0.09 | 33/s |
| + tick numbers off | 63.0 | 1.0 | 1.3 | 0.06 | 33/s |
| + big number off | 63.0 | 1.0 | 1.3 | 0.01 | 33/s |
| + readouts off | 63.6 | 1.0 | 1.2 | 0.01 | 33/s |
| + status bar off | 62.6 | 0.9 | 1.2 | 0.00 | 33/s |
| everything off | 62.2 | 0.9 | 1.2 | 0.01 | 33/s |
| everything back on | 63.0 | 1.0 | 1.3 | 0.09 | 33/s |

Flat, within noise, in both directions. Nothing is render-bound any more:
taking every redraw group away buys ~1 fps, which is measurement noise.

**Read the `fps` column carefully.** It counts loop iterations, not painted
frames. When the loop was render-bound the two were the same number (58.8 ms
of work per iteration → 17 fps), which is what made the 16.4 → 33 fps
acceptance test meaningful. Now the loop finishes in 16 ms, well inside the
33 ms UI period, so the loop rate saturates at ~63/s while the glass is served
by `ui::tick()` at `kUiPeriodMs` = 33 ms, i.e. **~30 painted frames per second
against a 33 Hz panel scan** — one frame behind the panel, set by the UI
period rather than by drawing. The measured render itself is 0.8–1.0 ms per
loop (1.1–1.4 ms for the whole loop body), down from 58.8 ms.

Flash went *down*: 83.1% → 82.3% (1 618 742 B of 1 966 080). RAM 46.8%
(153 492 / 327 680) — the deleted arc, needle, hub and tick code outweighed
the two new fonts.

## What is not proved

- Nothing here has been seen on real CAN data. The bench has no vehicle bus,
  so every fast field reads `--`; the mock-up (`docs/mockups/g-fullwidth.png`,
  `tools/mockup_dash.py`) is what the layout is checked against, and the
  in-car check is `e` on the console (ESP-NOW vs HTTP side by side).
- Boost is the field most likely to disagree once real data arrives: the C6
  reports it as mbar above ambient, and the tile's bar is scaled by `boostMax`.
- The `fps` counter above is a loop rate, not a paint rate. If the painted
  rate ever needs to be a number on the console rather than derived from
  `kUiPeriodMs`, it needs its own counter.
