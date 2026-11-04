#!/usr/bin/env python3
"""Capture the S3 dash serial console.

Pulses the CH340's RTS line to hard-reset the board, then prints everything
the firmware writes. Read-only apart from the reset line - it never puts the
board into download mode, so it is safe to run against a board that is
already flashed.

    python3 tools/serial_capture.py [seconds] [port] [baud]
"""
import sys
import time

import serial

SECONDS = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
PORT = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyUSB0"
BAUD = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

ser = serial.Serial(PORT, BAUD, timeout=0.2)

# Hard reset: RTS low then released. DTR must stay de-asserted so the board
# boots normally instead of entering the ROM download loader.
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.15)
ser.setRTS(False)
time.sleep(0.05)
ser.reset_input_buffer()

print("--- serial capture: %s @ %d, %.0f s ---" % (PORT, BAUD, SECONDS), flush=True)
deadline = time.monotonic() + SECONDS
while time.monotonic() < deadline:
    data = ser.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
print("\n--- end of capture ---")
ser.close()
