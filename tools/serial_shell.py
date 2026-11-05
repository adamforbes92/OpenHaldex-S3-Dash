#!/usr/bin/env python3
"""Hold the console open, log it to a file, and send keys into it.

Opening this board's serial port pulses the auto-reset lines, so every tool
that opens it resets the ESP32 and clears anything it was holding in RAM (the
touch probe, for one). This process opens the port once and keeps it, appends
everything the board prints to a log file, and sends the contents of a command
file whenever one appears - so a gesture and the console can be interleaved
without a reset in between.

usage:  serial_shell.py <logfile> [seconds] [port] [baud]
        echo -n r > <logfile>.cmd        # sends "r"; the file is removed
        echo -n t > <logfile>.cmd        # resets the touch probe
"""
import os
import sys
import time

import serial

log_path = sys.argv[1] if len(sys.argv) > 1 else "/home/adam/.goose_tmp/console.log"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 900.0
port = sys.argv[3] if len(sys.argv) > 3 else "/dev/ttyUSB0"
baud = int(sys.argv[4]) if len(sys.argv) > 4 else 115200
cmd_path = log_path + ".cmd"

ser = serial.Serial(port, baud, timeout=0.1)
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
