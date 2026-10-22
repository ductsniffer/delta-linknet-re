#!/usr/bin/env python3
"""Converts a capture log into a BACnet MS/TP pcap (linktype 165), for Wireshark.

    python3 tools/usb/to_pcap.py captures/<session>.log [out.pcap]

Each packet holds the whole frame, 55 FF preamble included, as mstpcap does.
"""
import struct
import sys

import mstp

LINKTYPE_BACNET_MS_TP = 165


def main() -> None:
    log = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else log.rsplit(".", 1)[0] + ".pcap"
    n = bad = 0
    with open(out, "wb") as f:
        f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, LINKTYPE_BACNET_MS_TP))
        for fr in mstp.frames(log):
            sec = int(fr.t)
            usec = int(round((fr.t - sec) * 1e6))
            if usec == 1_000_000:
                sec, usec = sec + 1, 0
            f.write(struct.pack("<IIII", sec, usec, len(fr.raw), len(fr.raw)))
            f.write(fr.raw)
            n += 1
            bad += not fr.crc_ok
    print(f"{out}: {n} frames, {bad} invalid data CRCs")


if __name__ == "__main__":
    main()
