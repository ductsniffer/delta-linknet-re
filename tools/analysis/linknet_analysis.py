#!/usr/bin/env python3
"""Offline analysis of a phase 2 session: the ESP32's raw stream and console.

    ./scripts/analyze.ps1 <session> [--log captures/esp32-<session>.log] [--captures <dir>]

Inputs (captures/):
  esp32-<session>-raw-NNN.raw  raw.ps1: every bus byte with its time
                               (format in firmware/lib/linknet_core/RawCapture.h)
  esp32-<...>.log              console.ps1: decoded zone lines, markers
Outputs (captures/analysis-<session>/):
  bus.bin       the bytes, as tools/usb/capture.py writes them in phase 1
  bus.pcap      one packet per frame, linktype 165 (BACnet MS/TP), stamped
                with the host clock: opens in Wireshark
  frames.csv    one line per frame other than Token and PollForMaster (those
                are millions a day; bus.pcap has them): time, type, src, dst,
                length, CRC, service
  summary.txt   counts, errors, the timing statistics
  *.png / *.svg the plots

Runs in the analysis container (docker/Dockerfile.analysis); never on the host.
A day of bus is ~110 M bytes and ~13.5 M frames: bytes and frames live in
numpy arrays, not Python objects, or the container runs out of memory.
"""
import argparse
import array
import bisect
import collections
import csv
import datetime as dt
import functools
import glob
import os
import re
import struct
import sys

import matplotlib
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

CHAR_US = 10 * 1e6 / 76800  # one 8N1 character at 76 800 baud: 130.2 us
LINKTYPE_BACNET_MS_TP = 165
TYPES = {0: "Token", 1: "PollForMaster", 2: "ReplyToPFM", 3: "TestRequest",
         4: "TestResponse", 5: "DataExpReply", 6: "DataNotExpReply", 7: "ReplyPostponed"}
ZONES = {1: "bedroom (1)", 2: "living room (2)"}


# --------------------------------------------------------------------- CRCs
# The same as tools/usb/mstp.py (phase 1) and firmware/lib/linknet_core/Crc.cpp:
# ASHRAE 135 Annex G.
@functools.lru_cache(maxsize=4096)  # a few hundred distinct headers on this bus
def crc8_header(octets):
    crc = 0xFF
    for b in octets:
        x = crc ^ b
        x ^= (x << 1) ^ (x << 2) ^ (x << 3) ^ (x << 4) ^ (x << 5) ^ (x << 6) ^ (x << 7)
        crc = ((x & 0xFE) ^ ((x >> 8) & 1)) & 0xFF
    return (~crc) & 0xFF


def crc16_data(octets):
    crc = 0xFFFF
    for b in octets:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return (~crc) & 0xFFFF


# --------------------------------------------------------------- raw stream
RECORD = np.dtype([("t", "<u4"), ("v", "u1"), ("f", "u1")])


class Segment:
    """One raw file: one TCP connection to the board."""

    def __init__(self, path):
        with open(path, "rb") as fh:
            data = fh.read()
        nl1 = data.index(b"\n")
        nl2 = data.index(b"\n", nl1 + 1)
        host = data[:nl1].decode().split(" ", 1)[1]
        # Local wall time, naive, like the console log stamps.
        self.host_connect = dt.datetime.fromisoformat(host).replace(tzinfo=None)
        # build= holds spaces (__DATE__ __TIME__): a value runs to the next key.
        header = dict(re.findall(r"(\w+)=(.*?)(?= \w+=|$)", data[nl1 + 1:nl2].decode()))
        self.board_connect_us = int(header["now_us"])
        self.header = header
        self.path = path
        n = (len(data) - nl2 - 1) // 6
        rec = np.frombuffer(data, dtype=RECORD, count=n, offset=nl2 + 1)
        # The board sends the low 32 bits; its clock at connection anchors
        # them. Backlog records are older than now_us, later ones newer.
        t32 = rec["t"].astype(np.int64)
        self.times = np.empty(n, dtype=np.int64)   # board clock, us, unwrapped
        if n:
            base = self.board_connect_us - (self.board_connect_us - int(t32[0])) % (1 << 32)
            self.times[0] = base
            self.times[1:] = base + np.cumsum(np.diff(t32) % (1 << 32))
        self.values = rec["v"].tobytes()
        lost = (rec["f"] & 1).astype(bool)
        self.lost_before = np.nonzero(lost)[0]
        # lost_upto[k]: lost-before flags on bytes 0..k. A difference of two
        # entries says whether bytes went missing between two positions.
        self.lost_upto = np.cumsum(lost, dtype=np.int32)

    def host_time(self, board_us):
        return self.host_connect + dt.timedelta(microseconds=int(board_us) - self.board_connect_us)


class Frames:
    """The frames of one segment, column-wise. Times are the board's: start =
    beginning of the first preamble byte (its end minus one character), end =
    end of the last byte (Esp32RxUart.h stamps the end of the stop bit)."""

    def __init__(self, seg, off, size, ftype, dst, src, hdr_ok, data_ok, service):
        self.off = np.frombuffer(off, dtype=np.int64)
        self.size = np.frombuffer(size, dtype=np.int32)
        self.type = np.frombuffer(ftype, dtype=np.uint8)
        self.dst = np.frombuffer(dst, dtype=np.uint8)
        self.src = np.frombuffer(src, dtype=np.uint8)
        self.hdr_ok = np.frombuffer(hdr_ok, dtype=np.uint8).astype(bool)
        self.data_ok = np.frombuffer(data_ok, dtype=np.uint8).astype(bool)
        self.service = np.frombuffer(service, dtype=np.int16)
        self.start = seg.times[self.off] - CHAR_US
        self.end = seg.times[self.off + self.size - 1]
        self.seg = seg

    def __len__(self):
        return len(self.off)

    def raw(self, k):
        return self.seg.values[self.off[k]:self.off[k] + self.size[k]]


def split_frames(seg):
    v = seg.values
    lost = seg.lost_before.tolist()
    off, size = array.array("q"), array.array("i")
    ftype, dst, src = array.array("B"), array.array("B"), array.array("B")
    hdr_ok_a, data_ok_a, service_a = array.array("B"), array.array("B"), array.array("h")
    junk, i, n = 0, 0, len(v)
    while i + 8 <= n:
        if v[i] != 0x55 or v[i + 1] != 0xFF:
            junk += 1
            i += 1
            continue
        hdr = v[i + 2:i + 7]
        if crc8_header(hdr) != v[i + 7]:
            off.append(i), size.append(8), ftype.append(hdr[0]), dst.append(hdr[1]), src.append(hdr[2])
            hdr_ok_a.append(0), data_ok_a.append(0), service_a.append(-1)
            i += 2
            continue
        length = hdr[3] << 8 | hdr[4]
        end = i + 8 + (length + 2 if length else 0)
        if end > n:
            break
        h = bisect.bisect_right(lost, i)
        if h < len(lost) and lost[h] < end:
            junk += end - i  # a hole inside the frame: not a frame
            i = end
            continue
        data_ok, service = 1, -1
        if length:
            data = v[i + 8:i + 8 + length]
            data_ok = int(crc16_data(data) == (v[end - 2] | v[end - 1] << 8))
            if hdr[0] in (5, 6) and len(data) > 7 and data[2:6] == b"\x10\x04\x09\x08":
                service = data[7]
        off.append(i), size.append(end - i), ftype.append(hdr[0]), dst.append(hdr[1]), src.append(hdr[2])
        hdr_ok_a.append(1), data_ok_a.append(data_ok), service_a.append(service)
        i = end
    return Frames(seg, off, size, ftype, dst, src, hdr_ok_a, data_ok_a, service_a), junk


def time_sync(raw):
    """BACnet TimeSynchronization (unconfirmed service 6), as the controller
    broadcasts it every hour: date tag a4 (year-1900, month, day, weekday),
    time tag b4 (hour, minute, second, hundredths). None for anything else."""
    data = raw[8:-2]
    if len(data) < 14 or data[2:4] != b"\x10\x06" or data[4] != 0xA4 or data[9] != 0xB4:
        return None
    y, mo, d, _, h, mi, s, cs = data[5:9] + data[10:14]
    try:
        return dt.datetime(1900 + y, mo, d, h, mi, s, cs * 10_000)
    except ValueError:
        return None


# ------------------------------------------------------------------ console
LINE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d+) (.*)$")
ZONE = re.compile(r"Z([12]) \w+ in=(\S+) sp=(\S+) out=(\S+) fan=(\S+) on=(\S+) adj=(\S+) raw=(\S+) \(")
KEY = re.compile(r"Z([12]) \w+ key=(\w+)")


def read_console(path):
    zones = {1: [], 2: []}
    keys, marks = [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = LINE.match(line.rstrip("\n"))
        if not m:
            continue
        when = dt.datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f")
        text = m.group(2)
        # "# MARKER", or "# MARQUEUR" in the French logs of the first sessions.
        mark = re.match(r"# (?:MARKER|MARQUEUR) (.*)", text)
        if mark:
            marks.append((when, mark.group(1)))
            continue
        z = ZONE.search(text)
        if z:
            def num(s):
                try:
                    return float(s)
                except ValueError:
                    return None
            zones[int(z.group(1))].append((when, *[num(z.group(k)) for k in range(2, 9)]))
            continue
        k = KEY.search(text)
        if k:
            keys.append((when, int(k.group(1)), k.group(2)))
    return zones, keys, marks


# ----------------------------------------------------------------- analysis
def describe(name, values, unit="us"):
    if len(values) == 0:
        return f"{name:<28} n=0"
    s = np.sort(np.asarray(values, dtype=np.float64))

    def pct(p):
        return s[min(len(s) - 1, int(p / 100 * len(s)))]

    return (f"{name:<28} n={len(s):<7} min={s[0]:9.0f} p50={pct(50):9.0f} "
            f"p99={pct(99):9.0f} max={s[-1]:9.0f} mean={s.mean():9.0f} {unit}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("session", help="e.g. 20261020-etape4")
    ap.add_argument("--log", help="console log (default: captures/esp32-<session>.log)")
    ap.add_argument("--captures", default="captures")
    a = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(a.captures, f"esp32-{a.session}-raw-*.raw")))
    if not paths:
        sys.exit(f"no raw files for {a.session}")
    out = os.path.join(a.captures, f"analysis-{a.session}")
    os.makedirs(out, exist_ok=True)
    log = a.log or os.path.join(a.captures, f"esp32-{a.session}.log")

    segments = [Segment(p) for p in paths]
    all_frames, report = [], []
    total_bytes = total_junk = 0
    report.append(f"session {a.session}: {len(segments)} raw file(s)")
    for s in segments:
        frames, junk = split_frames(s)
        total_bytes += len(s.values)
        total_junk += junk
        dur = (s.times[-1] - s.times[0]) / 1e6 if len(s.times) else 0
        report.append(f"  {os.path.basename(s.path)}: {len(s.values)} bytes over {dur:.1f} s, "
                      f"{len(frames)} frames, lost-before flags {len(s.lost_before)}, "
                      f"board header: {' '.join(f'{k}={v}' for k, v in s.header.items() if k != 'build')}")
        all_frames.append(frames)

    nonempty = [fr for fr in all_frames if len(fr)]
    first = min(fr.seg.host_time(fr.start[0]) for fr in nonempty)
    last = max(fr.seg.host_time(fr.start[-1]) for fr in nonempty)
    span = (last - first).total_seconds()
    n_frames = sum(len(fr) for fr in all_frames)
    hdr_bad = sum(int((~fr.hdr_ok).sum()) for fr in all_frames)
    data_bad = sum(int((fr.hdr_ok & ~fr.data_ok).sum()) for fr in all_frames)
    report.append(f"time covered: {first:%Y-%m-%d %H:%M:%S} .. {last:%Y-%m-%d %H:%M:%S} ({span / 60:.1f} min, "
                  f"with gaps between files)")
    report.append(f"bytes {total_bytes}, frames {n_frames}, header CRC errors {hdr_bad}, "
                  f"data CRC errors {data_bad}, bytes outside frames {total_junk}")
    by_type, svc = collections.Counter(), collections.Counter()
    for fr in all_frames:
        # PollForMaster destinations pooled: one per address 3..127.
        dst = np.where(fr.type == 1, 255 + 1, fr.dst.astype(np.int32))
        keys, counts = np.unique(np.stack([fr.type.astype(np.int32), fr.src, dst]), axis=1, return_counts=True)
        for (t, sr, de), c in zip(keys.T, counts):
            by_type[(TYPES.get(int(t), int(t)), int(sr), "*" if de == 256 else int(de))] += int(c)
        m = fr.service >= 0
        if not m.any():
            continue
        keys, counts = np.unique(np.stack([fr.service[m], fr.src[m], fr.dst[m]]), axis=1, return_counts=True)
        for (sv, sr, de), c in zip(keys.T, counts):
            svc[(int(sv), int(sr), int(de))] += int(c)
    report.append("frames by type, source, destination (PollForMaster destinations pooled):")
    for k, n in sorted(by_type.items(), key=lambda kv: -kv[1]):
        report.append(f"  {k[0]:<16} {k[1]:>3} -> {k[2]!s:<4} {n}")
    report.append("Delta private transfer services (service, source, destination):")
    for k, n in sorted(svc.items()):
        report.append(f"  SN{k[0]:<3} {k[1]} -> {k[2]}  {n}")

    # bus.bin, bus.pcap, frames.csv
    with open(os.path.join(out, "bus.bin"), "wb") as fb:
        for s in segments:
            fb.write(s.values)
    syncs = []
    with open(os.path.join(out, "bus.pcap"), "wb") as fp, \
            open(os.path.join(out, "frames.csv"), "w", newline="") as fc:
        fp.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, LINKTYPE_BACNET_MS_TP))
        w = csv.writer(fc)
        w.writerow(["host_time", "board_start_us", "board_end_us", "type", "src", "dst", "data_len",
                    "header_crc_ok", "data_crc_ok", "service", "hex"])
        for fr in all_frames:
            s = fr.seg
            epoch0 = s.host_connect.timestamp()
            ts = epoch0 + (fr.start - s.board_connect_us) / 1e6
            sec = np.floor(ts).astype(np.int64)
            usec = np.minimum(np.round((ts - sec) * 1e6).astype(np.int64), 999_999)
            buf = bytearray()
            for k in range(len(fr)):
                raw = fr.raw(k)
                buf += struct.pack("<IIII", sec[k], usec[k], len(raw), len(raw))
                buf += raw
                if len(buf) > (1 << 22):
                    fp.write(buf)
                    buf.clear()
            fp.write(buf)
            for k in np.nonzero((fr.type > 1) | ~fr.hdr_ok)[0]:
                raw = fr.raw(k)
                host = s.host_time(fr.start[k])
                w.writerow([host.isoformat(timespec="microseconds"), int(fr.start[k]), int(fr.end[k]),
                            TYPES.get(int(fr.type[k]), int(fr.type[k])), int(fr.src[k]), int(fr.dst[k]),
                            max(0, len(raw) - 10), int(fr.hdr_ok[k]), int(fr.data_ok[k]),
                            "" if fr.service[k] < 0 else int(fr.service[k]), raw.hex(" ")])
                if fr.dst[k] == 255 and fr.data_ok[k]:
                    ctl = time_sync(raw)
                    if ctl is not None:
                        syncs.append((host, int(fr.src[k]), ctl))

    # Timing, within each segment only (no gap spans two connections), and
    # never across bytes the board's ring dropped (RawCapture.h): that would
    # read as a long silence of the bus.
    gaps = collections.defaultdict(list)       # after a frame of this kind -> next start
    rotation = collections.defaultdict(list)   # token sent by station n, to the next one
    acks = collections.defaultdict(list)       # SN13/14 0->n end -> SN15 n->0 start
    inframe = []                               # silence between bytes of one frame
    for fr in all_frames:
        if len(fr) < 2:
            continue
        lu = fr.seg.lost_upto
        first_lu = lu[fr.off]
        last_lu = lu[fr.off + fr.size - 1]
        clean = (first_lu[1:] - last_lu[:-1]) == 0      # nothing lost between k and k+1
        g = fr.start[1:] - fr.end[:-1]
        pt, ps, pd = fr.type[:-1], fr.src[:-1], fr.dst[:-1]
        for d in np.unique(pd[clean & (pt == 0)]):
            gaps[f"after Token to {d}"].append(g[clean & (pt == 0) & (pd == d)])
        gaps["after PollForMaster (silence)"].append(g[clean & (pt == 1)])
        for sr in np.unique(ps[clean & (pt == 6)]):
            gaps[f"after data from {sr}"].append(g[clean & (pt == 6) & (ps == sr)])
        for sr in np.unique(fr.src[fr.type == 0]):
            idx = np.nonzero((fr.type == 0) & (fr.src == sr))[0]
            ok = (first_lu[idx[1:]] - first_lu[idx[:-1]]) == 0
            rotation[int(sr)].append((fr.start[idx[1:]] - fr.start[idx[:-1]])[ok])
        pending = {}
        for k in np.nonzero(fr.service >= 0)[0]:
            sv, sr, de = int(fr.service[k]), int(fr.src[k]), int(fr.dst[k])
            if sv in (13, 14) and sr == 0:
                pending[de] = (fr.end[k], sv, last_lu[k])
            elif sv == 15 and de == 0 and sr in pending:
                end, psv, plu = pending.pop(sr)
                if first_lu[k] == plu:
                    acks[(sr, psv)].append(fr.start[k] - end)
        # In-frame silences: every byte but the first of each frame.
        n = len(fr.seg.times)
        mark = np.zeros(n + 1, dtype=np.int32)
        np.add.at(mark, fr.off + 1, 1)
        np.add.at(mark, fr.off + fr.size, -1)
        inside = np.cumsum(mark[:n]) > 0
        inframe.append((np.diff(fr.seg.times) - CHAR_US)[inside[1:]])
    gaps = {k: np.concatenate(v) for k, v in gaps.items()}
    rotation = {k: np.concatenate(v) for k, v in rotation.items()}
    inframe = np.concatenate(inframe) if inframe else np.array([])

    report.append("")
    report.append("timing (us), from the board's byte stamps; a gap runs from the end of the")
    report.append("last byte of a frame to the start of the first byte of the next:")
    for k in sorted(gaps):
        report.append("  " + describe(k, gaps[k]))
    for n in sorted(rotation):
        report.append("  " + describe(f"token rotation seen at {n}", rotation[n]))
    for (n, sv), v in sorted(acks.items()):
        report.append("  " + describe(f"SN{sv} 0->{n} to SN15 ack", v))
    report.append("  " + describe("silence inside frames", inframe))
    busy = sum(int(fr.size.sum()) for fr in all_frames) * CHAR_US / 1e6
    seg_time = sum((s.times[-1] - s.times[0]) / 1e6 for s in segments if len(s.times))
    report.append(f"bus load: {busy / seg_time * 100:.1f} % of the time ({total_bytes / seg_time:.0f} bytes/s)")

    if syncs:
        report.append("")
        report.append("TimeSynchronization broadcasts (controller clock vs host clock at reception):")
        for host, sr, ctl in syncs:
            report.append(f"  {host:%Y-%m-%d %H:%M:%S.%f} from {sr}: {ctl:%a %Y-%m-%d %H:%M:%S.%f}"[:-4]
                          + f"  ({(ctl - host).total_seconds():+.2f} s)")

    # ---------------------------------------------------------------- plots
    plt.rcParams.update({"figure.dpi": 110, "font.size": 9, "axes.grid": True, "grid.alpha": 0.3})

    fig, axes = plt.subplots(2, 2, figsize=(11, 7))
    ax = axes[0][0]
    for k in sorted(gaps):
        if k.startswith("after Token"):
            ax.hist(gaps[k] / 1000, bins=80, alpha=0.6, label=k)
    ax.set_title("Token hand-over: silence before the next frame")
    ax.set_xlabel("ms")
    ax.set_ylabel("frames")
    ax.set_yscale("log")
    ax.legend()
    ax = axes[0][1]
    for n in sorted(rotation):
        ax.hist(rotation[n] / 1000, bins=100, alpha=0.5, label=f"station {n}")
    ax.set_title("Token rotation time (one full round)")
    ax.set_xlabel("ms")
    ax.set_yscale("log")
    ax.legend()
    ax = axes[1][0]
    for (n, sv), v in sorted(acks.items()):
        ax.hist(np.asarray(v) / 1000, bins=40, alpha=0.6, label=f"thermostat {n}, after SN{sv}")
    ax.set_title("Thermostat acknowledgement delay (SN15)")
    ax.set_xlabel("ms")
    ax.legend()
    ax = axes[1][1]
    ax.hist(inframe, bins=60)
    ax.set_title("Silence between bytes inside a frame")
    ax.set_xlabel("us beyond one character time")
    ax.set_yscale("log")
    fig.tight_layout()
    for ext in ("png", "svg"):
        fig.savefig(os.path.join(out, f"timing.{ext}"))
    plt.close(fig)

    # A 200 ms slice of the bus: who talks when.
    fr0 = max(all_frames, key=len)
    mid = fr0.start[len(fr0) // 2]
    fig, ax = plt.subplots(figsize=(11, 2.8))
    colors = {0: "tab:blue", 1: "tab:gray", 6: "tab:red"}
    for k in np.nonzero((fr0.start >= mid) & (fr0.start < mid + 200_000))[0]:
        ax.barh(int(fr0.src[k]), (fr0.end[k] - fr0.start[k]) / 1000, left=(fr0.start[k] - mid) / 1000,
                color=colors.get(int(fr0.type[k]), "tab:green"), height=0.6)
    ax.set_yticks([0, 1, 2])
    ax.set_yticklabels(["controller 0", "thermostat 1", "thermostat 2"])
    ax.set_xlabel("ms")
    ax.set_title("200 ms of bus traffic (blue: Token, gray: PollForMaster, red: data)")
    fig.tight_layout()
    for ext in ("png", "svg"):
        fig.savefig(os.path.join(out, f"timeline.{ext}"))
    plt.close(fig)

    if os.path.exists(log):
        zones, keys, marks = read_console(log)
        fig, axes = plt.subplots(3, 1, figsize=(11, 8), sharex=True)
        for z, rows in zones.items():
            rows = [r for r in rows if first - dt.timedelta(minutes=5) <= r[0] <= last + dt.timedelta(minutes=5)]
            if not rows:
                continue
            tt = [r[0] for r in rows]
            axes[0].step(tt, [r[1] for r in rows], where="post", label=f"indoor, {ZONES[z]}")
            axes[0].step(tt, [r[2] for r in rows], where="post", linestyle="--", label=f"setpoint, {ZONES[z]}")
            axes[2].step(tt, [r[7] for r in rows], where="post", label=f"raw sensor, {ZONES[z]}")
            if z == 1:
                axes[1].step(tt, [r[3] for r in rows], where="post", color="tab:green", label="outdoor")
        for when, z, key in keys:
            if first - dt.timedelta(minutes=5) <= when <= last + dt.timedelta(minutes=5):
                axes[0].axvline(when, color="tab:red", alpha=0.4, linewidth=0.8)
                axes[0].annotate(f"{key} {z}", (when, 1), xycoords=("data", "axes fraction"),
                                 rotation=90, va="top", fontsize=7, color="tab:red")
        axes[0].set_ylabel("deg C")
        axes[1].set_ylabel("deg C")
        axes[2].set_ylabel("raw units")
        for ax in axes:
            ax.legend(loc="best", fontsize=7)
        axes[0].set_title("What the controller displays (SN13) and what the thermostats measure (SN12)")
        fig.autofmt_xdate()
        fig.tight_layout()
        for ext in ("png", "svg"):
            fig.savefig(os.path.join(out, f"zones.{ext}"))
        plt.close(fig)
        report.append("")
        report.append(f"console {log}: {sum(len(v) for v in zones.values())} zone lines, {len(keys)} keys, "
                      f"{len(marks)} markers")

    with open(os.path.join(out, "summary.txt"), "w") as f:
        f.write("\n".join(report) + "\n")
    print("\n".join(report))
    print(f"\nwritten to {out}/")


if __name__ == "__main__":
    main()
