#!/usr/bin/env python3
"""Bench aid: reset the board, wait for it to boot, send a console command
and print everything the board says back (read/write, unlike
serial_capture.py which is deliberately read-only).

usage: serial_cmd.py [command] [wait_s] [port] [baud] [send_after_s]
"""
import sys, time
import serial

cmd = sys.argv[1] if len(sys.argv) > 1 else ""
wait_s = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
port = sys.argv[3] if len(sys.argv) > 3 else "/dev/ttyUSB0"
baud = int(sys.argv[4]) if len(sys.argv) > 4 else 115200
send_after = float(sys.argv[5]) if len(sys.argv) > 5 else max(0.0, wait_s - 6.0)

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
deadline = start + wait_s
sent = False
while time.time() < deadline:
    line = ser.readline().decode("utf-8", "replace")
    if line:
        print(line.rstrip())
    if not sent and cmd and time.time() - start >= send_after:
        ser.write(cmd.encode())
        ser.flush()
        sent = True
        print("--- sent %r ---" % cmd)
ser.close()
