#!/usr/bin/env python3
"""Read the console.

Opening this board's console port reboots it. That is measured, not assumed:
with a 12 s gap between two runs the second run still begins with a full boot
banner, so the reset comes from the kernel asserting DTR/RTS as the port is
opened - before any userspace code can set the lines. HUPCL is cleared and the
lines are dropped on open, which keeps the *close* from pulsing them as well.

What this tool is for, given that: it opens once and holds the port, so a whole
session of console keys and gestures is not reset halfway through. Use
serial_capture.py when a reset is what you want.

usage: serial_watch.py [seconds] [port] [baud]
"""
import os
import sys
import time

import serial
import termios

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
port = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyUSB0"
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

# The board's auto-reset circuit watches DTR/RTS. Clearing HUPCL *before*
# opening stops the driver asserting those lines on open, which is what makes
# the difference between reading this board's console and rebooting it.
def _clear_hupcl(port):
    try:
        fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError:
        return
    try:
        attrs = termios.tcgetattr(fd)
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
    except (termios.error, OSError):
        pass
    finally:
        os.close(fd)

ser = serial.Serial()
_clear_hupcl(port)
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
