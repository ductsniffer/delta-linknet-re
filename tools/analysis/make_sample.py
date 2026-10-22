#!/usr/bin/env python3
"""Cuts a time window out of a raw ESP32 recording and its console log, for
publication: re-anchors the raw header on the first kept record, and drops
the console's network lines (they carry LAN addresses).

    python tools/analysis/make_sample.py RAW LOG START END OUT_PREFIX
    START/END: local wall time, "YYYY-MM-DD HH:MM:SS"
"""
import datetime as dt
import re
import sys

import numpy as np

raw_path, log_path, start, end, out = sys.argv[1:6]
t0 = dt.datetime.fromisoformat(start)
t1 = dt.datetime.fromisoformat(end)

data = open(raw_path, "rb").read()
nl1 = data.index(b"\n")
nl2 = data.index(b"\n", nl1 + 1)
host_line = data[:nl1].decode()
host = dt.datetime.fromisoformat(host_line.split(" ", 1)[1])
header = data[nl1 + 1:nl2].decode()
now_us = int(re.search(r"now_us=(\d+)", header).group(1))
rec = np.frombuffer(data, dtype=np.dtype([("t", "<u4"), ("v", "u1"), ("f", "u1")]),
                    count=(len(data) - nl2 - 1) // 6, offset=nl2 + 1)
t32 = rec["t"].astype(np.int64)
times = np.empty(len(t32), dtype=np.int64)
times[0] = now_us - (now_us - int(t32[0])) % (1 << 32)
times[1:] = times[0] + np.cumsum(np.diff(t32) % (1 << 32))
naive = host.replace(tzinfo=None)
us0 = now_us + int((t0 - naive).total_seconds() * 1e6)
us1 = now_us + int((t1 - naive).total_seconds() * 1e6)
keep = np.nonzero((times >= us0) & (times < us1))[0]
a, b = int(keep[0]), int(keep[-1]) + 1
first_us = int(times[a])
new_host = host + dt.timedelta(microseconds=first_us - now_us)
new_header = re.sub(r"now_us=\d+", f"now_us={first_us}", header)
new_header = re.sub(r" pending=\d+ lost=\d+", " pending=0 lost=0", new_header)
with open(out + ".raw", "wb") as f:
    f.write(f"HOST {new_host.isoformat(timespec='milliseconds')}\n{new_header}\n".encode())
    f.write(rec[a:b].tobytes())
print(f"{out}.raw: {b - a} bytes, {new_host} .. +{(int(times[b - 1]) - first_us) / 1e6:.1f} s")

kept = 0
with open(out + ".log", "w", encoding="utf-8") as f:
    for line in open(log_path, encoding="utf-8", errors="replace"):
        stamp = line[:23]
        try:
            when = dt.datetime.strptime(stamp, "%Y-%m-%d %H:%M:%S.%f")
        except ValueError:
            continue
        if not t0 <= when < t1 or re.search(r" (NET|WIFI) ", line):
            continue
        f.write(line)
        kept += 1
print(f"{out}.log: {kept} lines")
