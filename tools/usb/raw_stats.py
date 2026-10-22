#!/usr/bin/env python3
"""First look at an unknown capture: byte rate, 55 FF preambles, MS/TP headers.
This is how the bus was first recognized as BACnet MS/TP.

    python3 tools/usb/raw_stats.py captures/<session>.log [max_seconds]

Does not touch the serial port.
"""
import collections
import sys

from mstp import TYPES, crc8_header


def read(log: str, tmax: float):
    t0 = None
    t_end = 0.0
    data = bytearray()
    for line in open(log, encoding="utf-8"):
        fields = line.split(" ", 3)
        if fields[1] == "#":
            continue
        t = float(fields[0])
        t0 = t0 or t
        if t - t0 > tmax:
            break
        data += bytes.fromhex(fields[3])
        t_end = t
    return bytes(data), t_end - t0


def main() -> None:
    log = sys.argv[1]
    tmax = float(sys.argv[2]) if len(sys.argv) > 2 else 1e12
    data, duration = read(log, tmax)
    print(f"{len(data)} bytes over {duration:.1f} s ({len(data) / duration:.0f} B/s)")
    print("start:", data[:64].hex(" "))

    pos = [i for i in range(len(data) - 1) if data[i] == 0x55 and data[i + 1] == 0xFF]
    print(f"55 FF preambles: {len(pos)}")

    ok, bad = 0, 0
    types = collections.Counter()
    pairs = collections.Counter()
    lengths = collections.Counter()
    for i in pos:
        h = data[i + 2:i + 8]
        if len(h) < 6:
            break
        if crc8_header(h[:5]) != h[5]:
            bad += 1
            continue
        ok += 1
        ft, dst, src, ln = h[0], h[1], h[2], (h[3] << 8) | h[4]
        name = TYPES.get(ft, f"0x{ft:02x}")
        types[name] += 1
        pairs[(src, dst, name)] += 1
        lengths[(name, ln)] += 1
    print(f"MS/TP headers with a valid CRC: {ok}, invalid: {bad}")
    print("types:", dict(types))
    print("source -> destination (type): count")
    for (src, dst, ft), n in sorted(pairs.items()):
        print(f"  {src:3d} -> {dst:3d}  {ft:20s} {n}")
    print("data length per type:")
    for (ft, ln), n in sorted(lengths.items()):
        print(f"  {ft:20s} {ln:4d} : {n}")


if __name__ == "__main__":
    main()
