#!/usr/bin/env python3
"""Bench aid: reset the board, then send a timed sequence of console
commands and print everything the board says back.

usage: serial_seq.py "30:n,36:d,45:s" [port] [baud]

The argument is a comma separated list of `<seconds_after_reset>:<command>`,
with `\n` meaning "just press enter". Commands are sent in list order at the
given offset from the reset, so a whole scene can be driven unattended.
"""
import sys, time
import serial

seq = sys.argv[1] if len(sys.argv) > 1 else ""
port = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyUSB0"
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

steps = []
for item in seq.split(","):
    item = item.strip()
    if not item:
        continue
    at, _, cmd = item.partition(":")
    steps.append((float(at), cmd.replace("\\n", "\n")))
steps.sort()
total = (steps[-1][0] + 12.0) if steps else 10.0

ser = serial.Serial()
ser.port = port
ser.baudrate = baud
ser.timeout = 0.2
ser.dtr = False
ser.rts = True
ser.open()
time.sleep(0.15)
ser.rts = False          # out of reset: the board boots normally
time.sleep(0.05)
ser.reset_input_buffer()

start = time.time()
while True:
    line = ser.readline().decode("utf-8", "replace")
    if line:
        print(line.rstrip())
    now = time.time() - start
    while steps and now >= steps[0][0]:
        _, cmd = steps.pop(0)
        ser.write(cmd.encode())
        ser.flush()
        print("--- sent %r at %.1fs ---" % (cmd, now))
    if now > total:
        break
ser.close()
