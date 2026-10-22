#!/usr/bin/env python3
"""Appends a time-stamped marker to the log of the running capture.py.

    python3 tools/usb/mark.py "START test 1: reference"

Does not open the serial port.
"""
import os
import sys
import time

CAPTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "captures")

with open(os.path.join(CAPTURES, "SESSION_USB")) as f:
    log = f.read().strip()

text = " ".join(sys.argv[1:]).replace("\n", " ")
t = time.time()
fd = os.open(log, os.O_WRONLY | os.O_APPEND)
os.write(fd, f"{t:.6f} # MARKER {text}\n".encode())
os.close(fd)
print(f"{time.strftime('%H:%M:%S', time.localtime(t))} {text}")
