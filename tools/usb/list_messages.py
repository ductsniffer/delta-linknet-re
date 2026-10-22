#!/usr/bin/env python3
"""Lists the Delta private messages (services 12 to 15), with the markers.

    python3 tools/usb/list_messages.py captures/<session>.log [after_marker] [--no-14-15]

  after_marker  text looked up in the markers; only what follows the last
                marker containing it is printed.
Does not touch the serial port.
"""
import sys
import time

import mstp

HEADER = bytes.fromhex("01001004090819")  # NPDU + UnconfirmedPrivateTransfer, vendor 8


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    no_poll = "--no-14-15" in sys.argv
    log = args[0]
    _, _, markers = mstp.read_log(log)
    t_from = 0.0
    if len(args) > 1:
        t_from = max(m.t for m in markers if args[1] in m.text)

    events = [(m.t, "#", m.text) for m in markers if m.t >= t_from]
    for fr in mstp.frames(log):
        if fr.t < t_from or not fr.data.startswith(HEADER):
            continue
        sn = fr.data[7]
        if no_poll and sn in (14, 15):
            continue
        p = fr.data[8:]
        text = f"{fr.src}->{fr.dst} SN{sn} {p.hex(' ')}"
        if sn == 13:
            fields = [c for c in p[3:-1].split(b"\x00") if c]
            ascii_ = [c.decode() for c in fields if all(48 <= b < 58 or b in (45, 46) for b in c)]
            text += f"  ascii={ascii_}"
        events.append((fr.t, "", text))
    for t, kind, text in sorted(events):
        h = time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t % 1 * 1000):03d}"
        print(f"{h} {'##### ' if kind else ''}{text}")


if __name__ == "__main__":
    main()
