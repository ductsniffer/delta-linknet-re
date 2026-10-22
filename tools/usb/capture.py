#!/usr/bin/env python3
"""Passive capture of the RS-485 bus (LINKnet), READ ONLY, through a USB
RS-485 adapter with automatic direction control.

This script never calls write() on the port: any byte written would go out
on the bus, since the adapter switches direction by itself.

    python3 tools/usb/capture.py [--port /dev/ttyUSB0] [--session NAME]

Writes into captures/ at the root of the repository:
  <session>.bin  raw bytes, in the order received
  <session>.log  text log, one line per read:
                   <time.time()> <offset in .bin> <n bytes> <hex>
                 markers (mark.py) are appended to it as:
                   <time.time()> # MARKER <text>
Needs pyserial.
"""
import argparse
import glob
import os
import signal
import sys
import time

import serial

BAUD = 76800
CAPTURES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "captures")


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/ttyUSB*"))
    if len(ports) != 1:
        sys.exit(f"no port, or more than one: {ports}; use --port")
    return ports[0]


def write_line(fd: int, text: str) -> None:
    # One os.write() per line on an O_APPEND file: mark.py can write into the
    # same log without the lines getting mixed.
    os.write(fd, text.encode())


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--port", default=None)
    p.add_argument("--session", default=time.strftime("%Y%m%d-%H%M%S"))
    args = p.parse_args()

    os.makedirs(CAPTURES, exist_ok=True)
    base = os.path.join(CAPTURES, args.session)
    port = args.port or find_port()

    s = serial.Serial()
    s.port = port
    s.baudrate = BAUD
    s.bytesize, s.parity, s.stopbits = 8, "N", 1
    s.xonxoff = s.rtscts = s.dsrdtr = False
    s.timeout = 0.5
    s.dtr = False
    s.rts = False
    s.open()

    flags = os.O_WRONLY | os.O_CREAT | os.O_APPEND
    fbin = os.open(base + ".bin", flags, 0o644)
    flog = os.open(base + ".log", flags, 0o644)
    with open(os.path.join(CAPTURES, "SESSION_USB"), "w") as f:
        f.write(os.path.abspath(base + ".log") + "\n")

    stopping = False

    def stop(*_):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    write_line(flog, f"{time.time():.6f} # START port={port} baud={BAUD} 8N1\n")
    print(f"capture -> {base}.bin / .log", flush=True)

    offset = 0
    last_total = time.time()
    while not stopping:
        data = s.read(1)
        if not data:
            continue
        n = s.in_waiting
        if n:
            data += s.read(n)
        t = time.time()
        os.write(fbin, data)
        write_line(flog, f"{t:.6f} {offset} {len(data)} {data.hex(' ')}\n")
        offset += len(data)
        if t - last_total >= 60:
            print(f"{time.strftime('%H:%M:%S')} {offset} bytes", flush=True)
            last_total = t

    write_line(flog, f"{time.time():.6f} # END bytes={offset}\n")
    s.close()
    os.close(fbin)
    os.close(flog)
    print(f"stopped, {offset} bytes", flush=True)


if __name__ == "__main__":
    main()
