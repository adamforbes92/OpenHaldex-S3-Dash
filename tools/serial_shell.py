#!/usr/bin/env python3
"""Hold the console open, log it to a file, and send keys into it.

Opening this board's serial port resets the ESP32 (measured: it happens on
open, at the kernel level, before this code runs). This process therefore opens
the port ONCE and keeps it, appending everything the board prints to a log
file and sending the contents of a command file whenever one appears - so a
whole session of gestures and console keys is not reset halfway through, which
is what matters when you are watching a live car.

usage:  serial_shell.py <logfile> [seconds] [port] [baud]
        echo -n r > <logfile>.cmd        # sends "r"; the file is removed
        echo -n t > <logfile>.cmd        # resets the touch probe
"""
import os
import sys
import time

import serial
import termios

log_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/ohx-console.log"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 900.0
port = sys.argv[3] if len(sys.argv) > 3 else "/dev/ttyUSB0"
baud = int(sys.argv[4]) if len(sys.argv) > 4 else 115200
cmd_path = log_path + ".cmd"

# Drop the auto-reset lines as the port opens, so this does not reboot the
# board. Set before open so they are never asserted, then again after, in case
# the driver ignored it. (Same trick as serial_watch.py.)
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
ser.timeout = 0.1
ser.dtr = False
ser.rts = False
ser.open()
ser.dtr = False
ser.rts = False
time.sleep(0.1)
ser.reset_input_buffer()

with open(log_path, "a", buffering=1) as log:
    log.write("\n=== serial_shell open %s ===\n" % time.strftime("%H:%M:%S"))
    end = time.monotonic() + secs
    while time.monotonic() < end:
        if os.path.exists(cmd_path):
            try:
                with open(cmd_path, "rb") as f:
                    keys = f.read()
                os.remove(cmd_path)
            except OSError:
                keys = b""
            if keys:
                ser.write(keys)
                ser.flush()
                log.write("=== sent %r at %s ===\n" % (keys, time.strftime("%H:%M:%S")))
        data = ser.read(4096)
        if data:
            log.write(data.decode("utf-8", "replace"))
    log.write("=== serial_shell closed ===\n")

ser.close()
