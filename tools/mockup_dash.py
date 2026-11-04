#!/usr/bin/env python3
"""Design mock-ups for the dash, drawn at the panel's real geometry.

Everything here is taken from the firmware so the pictures are not a lie:
the 800x480 grid, the palette in include/theme.h, the tile band
(y 253, h 197, columns 186/186/204/224), the 30 px status bar, the same
segment count a real bar would use, and "U" derived padding. Fonts are a
stand-in: the firmware uses LVGL's Montserrat, this uses Noto Sans Bold.

Candidates, all with boost on top (single colour) and revs below (zone
coloured), fattened from the first sketch:

    D  bars left, value stack right      big revs to the right of them

Variant E (big revs drawn on the rev bar) was rejected in favour of D, and
the lower BOOST tile is gone: boost now appears once, as the top bar plus
its value. The lower band is SPEED | TEMPERATURES | HALDEX.

Fonts are the ones the firmware will actually use: Montserrat Bold for the
two big numbers (generated into src/ui/fonts by tools/make_fonts.sh, since
LVGL ships Montserrat only up to 48 px) and Noto Sans Bold as a stand-in
for the smaller Montserrat sizes.

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
LABEL = (0x8a, 0x96, 0xa2)

# ---- layout (include/theme.h, evaluated for 800x480) -----------------
STATUS_H = 30
CONTENT_H = H - STATUS_H          # 450
TOP_H = (CONTENT_H * 56) // 100   # 252
BOTTOM_Y = TOP_H + 1              # 253
BOTTOM_H = CONTENT_H - TOP_H - 1  # 197
COL = [(0, 240), (240, 260), (500, 300)]   # SPEED | TEMPERATURES | HALDEX
PAD_H, PAD_V = 8, 7

# ---- bar design (the thing being mocked) -----------------------------
SEG, GAP = 40, 2          # 40 cells is what the real bar will use
BOOST_MAX = 2.5           # bar, absolute
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
    """Draw and check the box stayed inside the panel."""
    box = d.textbbox(xy, s, font=font(size), anchor=anchor)
    if box[0] < 0 or box[2] > W or box[1] < 0 or box[3] > H:
        WARN.append(f"'{s}' at {xy} {size}pt -> {box}")
    if box[3] > TOP_H and box[1] < BOTTOM_Y:
        WARN.append(f"'{s}' at {xy} {size}pt crosses into the tile band -> {box}")
    d.text(xy, s, font=font(size), fill=colour, anchor=anchor)
    return box


def text_in(d, box, s, size, colour, anchor="la"):
    """Draw, and check the box stayed inside its own rect."""
    got = d.textbbox(box[:2], s, font=font(size), anchor=anchor)
    if got[0] < box[0] or got[2] > box[0] + box[2]:
        WARN.append(f"'{s}' {size}pt x{box[0]}..{box[0]+box[2]} overflows -> {got}")
    return text(d, box[:2], s, size, colour, anchor)


def fit(s, wide, start=96, floor=20, step=2):
    """Largest size at or below start whose text fits in `wide` px."""
    for size in range(start, floor - 1, -step):
        if tw(s, size) <= wide:
            return size
    return floor


# ---------------------------------------------------------------- tiles
def tiles(d, speed="128", cool="92", oil="104"):
    """SPEED | TEMPERATURES | HALDEX. Boost is not here any more: it is the
    top bar plus the value beside it."""
    y, h = BOTTOM_Y, BOTTOM_H
    names = ["SPEED", "TEMPERATURES", "HALDEX"]
    for i, (x, w) in enumerate(COL):
        if i < len(COL) - 1:
            d.rectangle([x + w - 1, y, x + w - 1, y + h], fill=LINE)
        text(d, (x + PAD_H, y + PAD_V), names[i], 12, DIM)

    # SPEED
    text(d, (COL[0][0] + PAD_H - 2, y + 44), speed, 48, FG)
    text(d, (COL[0][0] + PAD_H + 4, y + h - 44), "km/h", 12, DIM)
    text(d, (COL[0][0] + PAD_H, y + h - 24), "peak 187", 14, DIM)

    # TEMPERATURES
    twid = COL[1][1] - 2 * PAD_H
    x2 = COL[1][0] + PAD_H
    text(d, (x2, y + 40), "COOLANT", 12, DIM)
    text(d, (x2 + twid, y + 30), cool, 22, FG, anchor="ra")
    d.rectangle([x2, y + 74, x2 + twid, y + 84], fill=TRACK)
    d.rectangle([x2, y + 74, x2 + int(twid * 0.58), y + 84], fill=GREEN)
    text(d, (x2, y + 104), "OIL", 12, DIM)
    text(d, (x2 + twid, y + 94), oil, 22, FG, anchor="ra")
    d.rectangle([x2, y + 138, x2 + twid, y + 148], fill=TRACK)
    d.rectangle([x2, y + 138, x2 + int(twid * 0.71), y + 148], fill=GREEN)

    # HALDEX
    x3 = COL[2][0] + PAD_H
    text(d, (x3, y + 36), "75:25", 28, FG)
    text(d, (x3, y + 70), "coupling closed", 14, DIM)
    text(d, (x3, y + 94), "LOCK REQ", 12, DIM)
    text(d, (COL[2][0] + COL[2][1] - PAD_H, y + 86), "30%", 22, FG, anchor="ra")
    text(d, (x3, y + 126), "LOCK ACT", 12, DIM)
    text(d, (COL[2][0] + COL[2][1] - PAD_H, y + 118), "28%", 22, CYAN, anchor="ra")
    text(d, (x3 + 4, y + 160), "THR", 12, FG)
    tw3 = COL[2][1] - 2 * PAD_H
    d.rectangle([x3, y + 176, x3 + tw3, y + 188], fill=TRACK)
    d.rectangle([x3, y + 176, x3 + int(tw3 * 0.42), y + 188], fill=GREEN)


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


def ticks(d, x, y, w, left="", right="x1000", frm=1, to=8):
    """Numbers along a bar; a label at an end takes that end's slot."""
    if left:
        frm += 1
        text(d, (x, y), left, 12, DIM)
    if right:
        to -= 1
    for n in range(frm, to + 1):
        text(d, (x + round((n - 1) / 7.0 * w), y), str(n), 14, DIM, anchor="ma")
    if right:
        text(d, (x + w - 4, y), right, 12, DIM, anchor="ra")


def revs(d, x, y, size, anchor="la", colour=FG, label=True):
    """The big number, with a small 'RPM' tag tucked against it."""
    box = d.textbbox((x, y), "6800", font=font(size), anchor=anchor)
    text(d, (x, y), "6800", size, colour, anchor=anchor)
    if label:
        if anchor[0] == "l":
            text(d, (box[2] + 10, box[3] - 20), "RPM", 18, DIM)
        else:
            text(d, (box[2], y + th("6800", size) + 8), "RPM", 18, DIM, anchor="ra")
    return box


def revs_over(d, x, y, size):
    """Big number drawn on top of a bar: dark halo so it reads on any colour."""
    for dx, dy in ((-2, 0), (2, 0), (0, -2), (0, 2), (-1, -1), (1, 1), (-1, 1), (1, -1)):
        d.text((x + dx, y + dy), "6800", font=font(size), fill=BG, anchor="la")
    text(d, (x, y), "6800", size, WHITE)


# --------------------------------------------------------------- variants
def variant_d(d, bc, bname):
    """Bars on the left, a value stack on the right."""
    lx, lw = 8, 512                     # left column of bars
    rx, rw = 520, 272                   # right value column
    d.rectangle([rx - 9, 0, rx - 9, TOP_H], fill=LINE)

    # boost bar, single colour, on top (no text on it: the right column
    # and the tick row carry the words, so nothing has to read on a lit bar)
    bh = 56
    bar(d, lx, 6, lw, bh, BOOST_NOW / BOOST_MAX, lambda c: bc, lambda c: TRACK)

    # rev bar, zone coloured, fat
    rh = 142
    bar(d, lx, 70, lw, rh, RPM_NOW / float(RPM_MAX), zone_colour, zone_unlit)
    ticks(d, lx, 218, lw)

    # right: a value stack, one header + one big number per row
    text(d, (rx, 8), "BOOST", 12, DIM)
    text(d, (rx + rw, 22), "1.55", fit("1.55", rw, 56), FG, anchor="ra")
    text(d, (rx + rw, 78), "bar", 14, DIM, anchor="ra")
    d.rectangle([rx, 104, rx + rw, 104], fill=LINE)
    text(d, (rx, 116), "RPM", 12, DIM)
    revs(d, rx + rw, 130, fit("6800", rw, 96), anchor="ra", label=False)
    return f"d-{bname}.png"





# ------------------------------------------------------------------- main
def build(fn, bc, bname, title):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    name = fn(d, bc, bname)
    tiles(d)
    status(d)
    img.save(os.path.join(OUT, name))
    print(f"  {name:24s} {title}")


def main():
    os.makedirs(OUT, exist_ok=True)
    print("mock-ups:")
    build(variant_d, WHITE, "white", "boost bar in white (chosen)")
    build(variant_d, CYAN, "cyan", "boost bar in cyan (for comparison)")
    if WARN:
        print("\nWARNINGS (element left its box):")
        for w in WARN:
            print("  " + w)
        sys.exit(1)
    print("\nall elements inside their boxes")


if __name__ == "__main__":
    main()
