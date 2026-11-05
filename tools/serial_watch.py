#!/usr/bin/env python3
"""Read the console WITHOUT resetting the board.

serial_capture.py pulses RTS to hard-reset, which is what you want when you
are watching a boot but not when the board is holding state you care about
(the touch probe, for instance). Here DTR/RTS are dropped as the port opens,
so nothing is pulsed; if the board did reset anyway, the boot banner will be
the first thing in the output, which is the tell.

usage: serial_watch.py [seconds] [port] [baud]
"""
import sys
import time

import serial

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
port = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyUSB0"
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

ser = serial.Serial()
ser.port = port
ser.baudrate = baud
ser.timeout = 0.2
# Set before open so the lines are never asserted, then again after, in case
# the driver ignored it.
ser.dtr = False
ser.rts = False
ser.open()
ser.dtr = False
ser.rts = False
time.sleep(0.05)
ser.reset_input_buffer()

out = []
end = time.monotonic() + secs
while time.monotonic() < end:
    data = ser.read(4096)
    if data:
        out.append(data.decode("utf-8", "replace"))
ser.close()

text = "".join(out)
print(text)
print("--- %d bytes ---" % len(text))
