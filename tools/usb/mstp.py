"""Offline BACnet MS/TP framing of a capture log written by capture.py.

Never touches the serial port: it only reads the files capture.py wrote.
The firmware's decoder (firmware/lib/linknet_core/Mstp.cpp) follows the same
algorithm, so both give the same frames on the same bytes.
"""
from dataclasses import dataclass
from typing import Iterator, List, Tuple

TYPES = {
    0: "Token", 1: "PollForMaster", 2: "ReplyToPFM", 3: "TestRequest",
    4: "TestResponse", 5: "DataExpReply", 6: "DataNotExpReply",
    7: "ReplyPostponed",
}


@dataclass
class Frame:
    t: float           # time.time() of the read that brought the last byte
    offset: int        # position of the preamble in the raw stream
    type: int
    dst: int
    src: int
    data: bytes
    crc_ok: bool       # data CRC (True when there is no data)
    raw: bytes         # whole frame, 55 FF preamble included

    @property
    def name(self) -> str:
        return TYPES.get(self.type, f"0x{self.type:02x}")


@dataclass
class Marker:
    t: float
    text: str


# ASHRAE 135, Annex G.
def crc8_header(octets: bytes) -> int:
    crc = 0xFF
    for b in octets:
        x = crc ^ b
        x ^= (x << 1) ^ (x << 2) ^ (x << 3) ^ (x << 4) ^ (x << 5) ^ (x << 6) ^ (x << 7)
        crc = ((x & 0xFE) ^ ((x >> 8) & 1)) & 0xFF
    return (~crc) & 0xFF


def crc16_data(octets: bytes) -> int:
    crc = 0xFFFF
    for b in octets:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return (~crc) & 0xFFFF


def read_log(log: str) -> Tuple[bytes, List[Tuple[int, float]], List[Marker]]:
    """Returns the raw stream, the (end offset, time) of each read, and the markers."""
    stream = bytearray()
    times: List[Tuple[int, float]] = []
    markers: List[Marker] = []
    for line in open(log, encoding="utf-8"):
        if not line.endswith("\n"):
            break  # last line still being written by capture.py
        fields = line.rstrip("\n").split(" ", 3)
        if fields[1] == "#":
            markers.append(Marker(float(fields[0]), line.split("# ", 1)[1].rstrip("\n")))
            continue
        stream += bytes.fromhex(fields[3])
        times.append((len(stream), float(fields[0])))
    return bytes(stream), times, markers


def frames(log: str) -> Iterator[Frame]:
    stream, times, _ = read_log(log)
    k = 0
    i = 0
    n = len(stream)
    while True:
        i = stream.find(b"\x55\xff", i)
        if i < 0 or i + 8 > n:
            return
        h = stream[i + 2:i + 8]
        if crc8_header(h[:5]) != h[5]:
            i += 1
            continue
        length = (h[3] << 8) | h[4]
        end = i + 8 + (length + 2 if length else 0)
        if end > n:
            return
        data = stream[i + 8:i + 8 + length]
        crc_ok = True
        if length:
            received = stream[i + 8 + length] | (stream[i + 9 + length] << 8)
            crc_ok = crc16_data(data) == received
        while times[k][0] < end:
            k += 1
        yield Frame(times[k][1], i, h[0], h[1], h[2], bytes(data), crc_ok, stream[i:end])
        i = end
