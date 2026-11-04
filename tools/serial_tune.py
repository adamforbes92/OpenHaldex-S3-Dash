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
    ("arcs off", "1"),
    ("needle off too", "2"),
    ("ticks off too", "3"),
    ("digits+shift off too", "4"),
    ("readouts off too", "5"),
    ("status bar off too", "6"),
    ("everything off", "9"),
    ("everything back on", "0"),
]

RE_RENDER = re.compile(r"render ([\d.]+) fps \| ([\d.]+) ms/loop \| ([\d.]+) ms/flush")
RE_TICK = re.compile(r"ms/tick: tach ([\d.]+) readouts ([\d.]+) conn ([\d.]+)")

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
    tick = RE_TICK.findall(text)
    mask = re.findall(r"mask (0x[0-9A-Fa-f]+)", text)
    use_fps = fps[-1] if fps else float("nan")
    use_loop = loop[-1] if loop else float("nan")
    t = tick[-1] if tick else ("?", "?", "?")
    m = mask[-1] if mask else "-"
    rows.append((label, use_fps, use_loop, t, m))
    print(f"{label:28s} fps={use_fps:5.1f} loop={use_loop:6.1f}ms "
          f"tach={t[0]:>6} readouts={t[1]:>6} conn={t[2]:>5} {m}", flush=True)

ser.close()

print("\n--- summary ---")
base = rows[0][1]
for label, fps, loop, t, m in rows:
    gain = (fps / base) if base and base == base and fps == fps else float("nan")
    print(f"{label:28s} {fps:5.1f} fps   x{gain:4.2f}")
