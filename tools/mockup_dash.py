#!/usr/bin/env python3
"""Design mock-ups for the dash, drawn at the panel's real geometry.

Everything here is taken from the firmware so the pictures are not a lie:
the 800x480 grid, the palette in include/theme.h, the 30 px status bar, the
same segment count a real bar would use, and "U" derived padding.

    G  one full-width rev bar, the big rev number under it, then the four
       original tiles: SPEED | BOOST | TEMPERATURES | HALDEX

History: A and B were retracted (the big number ran off the panel), D shipped
(two bars on the left, a value stack on the right, three tiles - no BOOST
tile because boost was on the bar), E was rejected in favour of D, and F (two
bars plus a large centred number in a 252 px band) could not be made to fit.
G is the answer to "the only full-width item is revs": the boost bar and the
value stack are gone, REVS spans the panel and its number gets its own row.

Fonts are the ones the firmware will actually use: Montserrat Bold for the big
number (generated into src/ui/fonts by tools/make_fonts.sh, since LVGL ships
Montserrat only up to 48 px) and Noto Sans Bold as a stand-in for the smaller
Montserrat sizes.

    python3 tools/mockup_dash.py           -> docs/mockups/*.png
"""
import os
import sys
from PIL import Image, ImageDraw, ImageFont

W, H = 800, 480
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "mockups")

# ---- palette (include/theme.h) ---------------------------------------
BG = (0, 0, 0)
PANEL = (0x0b, 0x0d, 0x10)
LINE = (0x1c, 0x21, 0x28)
FG = (0xe8, 0xed, 0xf2)
DIM = (0x7c, 0x88, 0x94)
GREEN = (0x22, 0xc5, 0x5e)
AMBER = (0xf5, 0x9e, 0x0b)
RED = (0xef, 0x44, 0x44)
CYAN = (0x22, 0xd3, 0xee)
WHITE = (0xf2, 0xf5, 0xf8)
TRACK = (0x14, 0x18, 0x1d)
LAMP_OFF = (0x0e, 0x11, 0x14)
LAMP_EDGE = (0x2a, 0x2f, 0x36)
LAMP_TXT_OFF = (0x3c, 0x44, 0x4d)
Z_GREEN = (0x12, 0x3a, 0x22)
Z_AMBER = (0x3a, 0x2a, 0x10)
Z_RED = (0x3a, 0x14, 0x14)

# ---- layout (include/theme.h, evaluated for 800x480) -----------------
STATUS_H = 30
CONTENT_H = H - STATUS_H          # 450

BAR_X, BAR_W = 0, W               # REVS is the only full-width item
REV_BAR_Y, REV_BAR_H = 6, 104     # fat: it is the one the eye tracks
TICK_Y = 114                      # scale numbers under the bar
RPM_NUM_Y = 132                   # the big number's own row, 132 .. 230
TILE_Y = 230
TILE_H = CONTENT_H - TILE_Y       # 220
# SPEED | BOOST | TEMPERATURES | HALDEX  (the four original sections)
COL = [(0, 186), (186, 186), (372, 204), (576, 224)]
PAD_H, PAD_V = 8, 7

# ---- bar design (the thing being mocked) -----------------------------
SEG, GAP = 40, 2          # 40 cells is what the real bar will use
BOOST_MAX = 2.5           # boost tile bar, absolute
BOOST_NOW = 1.55
RPM_NOW, RPM_MAX = 6800, 8000
RPM_AMBER, RPM_RED = 6000, 6800

_here = os.path.dirname(os.path.abspath(__file__))
FONT_PATH = os.path.join(_here, "fonts", "Montserrat-Bold.ttf")      # big numbers
FONT_SMALL_PATH = "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf"  # stand-in
_cache = {}
WARN = []


def font(size):
    if size not in _cache:
        path = FONT_PATH if size >= 56 else FONT_SMALL_PATH
        _cache[size] = ImageFont.truetype(path, size)
    return _cache[size]


def tw(s, size):
    b = font(size).getbbox(s)
    return b[2] - b[0]


def th(s, size):
    b = font(size).getbbox(s)
    return b[3] - b[1]


def text(d, xy, s, size, colour, anchor="la"):
    """Draw and check the box stayed inside the panel and off the tiles."""
    box = d.textbbox(xy, s, font=font(size), anchor=anchor)
    if box[0] < 0 or box[2] > W or box[1] < 0 or box[3] > H:
        WARN.append(f"'{s}' at {xy} {size}pt -> {box}")
    if box[3] > TILE_Y and box[1] < TILE_Y and not _in_tile(box):
        WARN.append(f"'{s}' at {xy} {size}pt crosses into the tile band -> {box}")
    d.text(xy, s, font=font(size), fill=colour, anchor=anchor)
    return box


def _in_tile(box):
    return any(box[0] >= x - 1 and box[2] <= x + w + 1 for x, w in COL)


def fit(s, wide, start=96, floor=20, step=2):
    """Largest size at or below start whose text fits in `wide` px."""
    for size in range(start, floor - 1, -step):
        if tw(s, size) <= wide:
            return size
    return floor


# ---------------------------------------------------------------- tiles
def tiles(d):
    """SPEED | BOOST | TEMPERATURES | HALDEX - the four original sections.
    Only REVS is full width; boost is a tile again, as it was before."""
    y, h = TILE_Y, TILE_H
    names = ["SPEED", "BOOST", "TEMPERATURES", "HALDEX"]
    for i, (x, w) in enumerate(COL):
        if i < len(COL) - 1:
            d.rectangle([x + w - 1, y, x + w - 1, y + h], fill=LINE)
        text(d, (x + PAD_H, y + PAD_V), names[i], 12, DIM)

    # ---- SPEED
    sx = COL[0][0]
    text(d, (sx + PAD_H - 2, y + 44), "128", 48, FG)
    text(d, (sx + PAD_H + 4, y + h - 44), "km/h", 12, DIM)
    text(d, (sx + PAD_H, y + h - 24), "peak 187", 14, DIM)

    # ---- BOOST (value, unit, its own small bar, peak)
    bw = COL[1][1] - 2 * PAD_H
    bx = COL[1][0] + PAD_H
    text(d, (bx, y + 40), "1.55", fit("1.55", bw, 56), FG)
    text(d, (bx, y + 96), "bar", 14, DIM)
    d.rectangle([bx, y + 128, bx + bw, y + 138], fill=TRACK)
    d.rectangle([bx, y + 128, bx + int(bw * BOOST_NOW / BOOST_MAX), y + 138], fill=WHITE)
    text(d, (bx, y + h - 24), "peak 1.8", 14, DIM)

    # ---- TEMPERATURES
    x2 = COL[2][0] + PAD_H
    x2w = COL[2][1] - 2 * PAD_H
    text(d, (x2, y + 44), "COOLANT", 12, DIM)
    text(d, (x2 + x2w, y + 34), "92", 22, FG, anchor="ra")
    d.rectangle([x2, y + 78, x2 + x2w, y + 88], fill=TRACK)
    d.rectangle([x2, y + 78, x2 + int(x2w * 0.58), y + 88], fill=GREEN)
    text(d, (x2, y + 120), "OIL", 12, DIM)
    text(d, (x2 + x2w, y + 110), "104", 22, FG, anchor="ra")
    d.rectangle([x2, y + 154, x2 + x2w, y + 164], fill=TRACK)
    d.rectangle([x2, y + 154, x2 + int(x2w * 0.71), y + 164], fill=GREEN)

    # ---- HALDEX
    x3 = COL[3][0] + PAD_H
    x3w = COL[3][1] - 2 * PAD_H
    text(d, (x3, y + 40), "75:25", 28, FG)
    text(d, (x3, y + 76), "coupling closed", 14, DIM)
    text(d, (x3, y + 102), "LOCK REQ", 12, DIM)
    text(d, (x3 + x3w, y + 94), "30%", 22, FG, anchor="ra")
    text(d, (x3, y + 136), "LOCK ACT", 12, DIM)
    text(d, (x3 + x3w, y + 128), "28%", 22, CYAN, anchor="ra")
    text(d, (x3 + 4, y + 170), "THR", 12, FG)
    d.rectangle([x3, y + 186, x3 + x3w, y + 198], fill=TRACK)
    d.rectangle([x3, y + 186, x3 + int(x3w * 0.42), y + 198], fill=GREEN)


# ------------------------------------------------------------ status bar
def status(d):
    y = H - STATUS_H
    d.rectangle([0, y, W, H], fill=PANEL)
    d.rectangle([0, y, W, y], fill=LINE)
    d.ellipse([9, y + 9, 20, y + 20], fill=GREEN)
    text(d, (26, y + 8), "LIVE", 16, GREEN)
    for i, name in enumerate(["BRAKE", "HAND", "TC", "ASR", "AWD"]):
        x = [92, 146, 200, 238, 280][i]
        w = [50, 50, 32, 40, 42][i]
        d.rectangle([x, y + 5, x + w, y + 25], fill=LAMP_OFF, outline=LAMP_EDGE)
        text(d, (x + w // 2, y + 15), name, 12, LAMP_TXT_OFF, anchor="mm")
    for x, w, name in [(516, 104, "RESET PEAKS"), (626, 56, "DAY"), (688, 104, "SETTINGS")]:
        d.rectangle([x, y + 3, x + w, y + 27], fill=(0x12, 0x16, 0x1b), outline=LAMP_EDGE)
        text(d, (x + w // 2, y + 15), name, 12, FG, anchor="mm")


# ------------------------------------------------------------------ bars
def bar(d, x, y, w, h, lit_frac, colour_at, unlit_at):
    """Segmented bar: cell i is lit when its centre is inside lit_frac."""
    for i in range(SEG):
        x0 = x + round(i * w / SEG)
        x1 = x + round((i + 1) * w / SEG) - GAP
        c = (i + 0.5) / SEG
        d.rectangle([x0, y, x1, y + h], fill=colour_at(c) if c <= lit_frac else unlit_at(c))


def zone_colour(c):
    if c < RPM_AMBER / float(RPM_MAX):
        return GREEN
    if c < RPM_RED / float(RPM_MAX):
        return AMBER
    return RED


def zone_unlit(c):
    if c < RPM_AMBER / float(RPM_MAX):
        return Z_GREEN
    if c < RPM_RED / float(RPM_MAX):
        return Z_AMBER
    return Z_RED


def ticks(d, y):
    """Same placement as syncGauge(): one number per 1000 rpm, centred on
    n/rpmMax of the bar, unit tag in the top slot at the right-hand end."""
    for n in range(1, 8):
        x = round(n * 1000.0 / RPM_MAX * BAR_W)
        text(d, (x, y), str(n), 14, DIM, anchor="ma")
    text(d, (BAR_W - 4, y), "x1000", 12, DIM, anchor="ra")


# --------------------------------------------------------------- variants
def variant_g(d):
    """One full-width rev bar, the big rev number under it, four tiles."""
    bar(d, BAR_X, REV_BAR_Y, BAR_W, REV_BAR_H, RPM_NOW / float(RPM_MAX),
        zone_colour, zone_unlit)
    ticks(d, TICK_Y)

    # The number gets a row of its own, centred, in the 96 px font the
    # firmware already ships (Montserrat Bold, generated).
    size = 96
    text(d, (W // 2, (RPM_NUM_Y + TILE_Y) // 2), "6800", size, FG, anchor="mm")
    return "g-fullwidth.png"


# ------------------------------------------------------------------- main
def build(fn, title):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    name = fn(d)
    tiles(d)
    status(d)
    img.save(os.path.join(OUT, name))
    print(f"  {name:24s} {title}")


def main():
    os.makedirs(OUT, exist_ok=True)
    print("mock-ups:")
    build(variant_g, "full-width REVS bar + big number, then SPEED|BOOST|TEMPS|HALDEX")
    if WARN:
        print("\nWARNINGS (element left its box):")
        for w in WARN:
            print("  " + w)
        sys.exit(1)
    print("\nall elements inside their boxes")


if __name__ == "__main__":
    main()
