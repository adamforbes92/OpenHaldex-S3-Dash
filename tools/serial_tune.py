#!/usr/bin/env python3
"""Measure dashboard render cost per redraw group, on the real panel.

Resets the board, then for each step sends a character to the firmware's
bring-up console (see handleSerial() in main.cpp) and reports the frame rate
and per-phase timings that the firmware prints every 5 s.

    python3 tools/serial_tune.py
"""
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
BAUD = 115200

# (label, characters to send)
STEPS = [
    ("baseline (everything on)", ""),
    ("rev bar off", "1"),
    ("tick numbers off too", "2"),
    ("big number off too", "3"),
    ("readouts off too", "4"),
    ("status bar off too", "5"),
    ("everything off", "9"),
    ("everything back on", "0"),
]

RE_RENDER = re.compile(
    r"render ([\d.]+) fps \| ([\d.]+) ms/loop, ([\d.]+) ms/busy of a ([\d.]+) ms period"
    r" \| ([\d.]+) ms/flush")
RE_TICK = re.compile(r"ms/tick: gauge ([\d.]+) readouts ([\d.]+) conn ([\d.]+)")
RE_VSYNC = re.compile(r"vsync ([\d.]+)/s")

ser = serial.Serial(PORT, BAUD, timeout=0.2)
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.15)
ser.setRTS(False)
time.sleep(0.05)
ser.reset_input_buffer()


def pump(seconds):
    """Read for `seconds`, return the decoded text."""
    out = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        data = ser.read(4096)
        if data:
            out.append(data.decode("utf-8", "replace"))
    return "".join(out)


print("waiting for boot ...", flush=True)
boot = pump(9.0)
print(boot[-400:])

rows = []
for label, keys in STEPS:
    if keys:
        ser.write(keys.encode())
        ser.flush()
        time.sleep(0.4)
    text = pump(11.0)  # at least two 5 s stat lines
    fps = [float(m.group(1)) for m in RE_RENDER.finditer(text)]
    loop = [float(m.group(2)) for m in RE_RENDER.finditer(text)]
    busy = [float(m.group(3)) for m in RE_RENDER.finditer(text)]
    flush = [float(m.group(5)) for m in RE_RENDER.finditer(text)]
    tick = RE_TICK.findall(text)
    mask = re.findall(r"mask (0x[0-9A-Fa-f]+)", text)
    vsync = [float(m.group(1)) for m in RE_VSYNC.finditer(text)]
    use_fps = fps[-1] if fps else float("nan")
    use_loop = loop[-1] if loop else float("nan")
    t = tick[-1] if tick else ("?", "?", "?")
    m = mask[-1] if mask else "-"
    use_busy = busy[-1] if busy else float("nan")
    use_flush = flush[-1] if flush else float("nan")
    use_vsync = vsync[-1] if vsync else float("nan")
    rows.append((label, use_fps, use_loop, t, m, use_busy, use_flush))
    print(f"{label:28s} fps={use_fps:5.1f} loop={use_loop:6.1f} busy={use_busy:6.1f} "
          f"flush={use_flush:6.1f} gauge={t[0]:>6} readouts={t[1]:>6} vsync={use_vsync:4.0f}/s "
          f"{m}", flush=True)

ser.close()

print("\n--- summary ---")
base = rows[0][1]
for label, fps, loop, t, m, busy, flush in rows:
    gain = (fps / base) if base and base == base and fps == fps else float("nan")
    print(f"{label:28s} {fps:5.1f} fps   x{gain:4.2f}")
