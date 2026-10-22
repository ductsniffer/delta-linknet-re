# Example frames (test vectors)

Complete frames copied from the capture of 2026-10-16 (`captures/phase1-usb/`): the `55 FF` preamble, a
6-byte header (frame type, destination, source, 2-byte big-endian data length, CRC-8), the data, then the
little-endian CRC-16 of the data.

The firmware's unit tests read this file directly (`firmware/test/test_frames/`), so the document and the
tests cannot drift apart. Keep the `##` titles and the code blocks as they are.
## Token — 1 → 2

```
55 ff 00 02 01 00 00 73
```

## PollForMaster — 2 → 3

```
55 ff 01 03 02 00 00 d7
```

## DataNotExpReply / service 14 — 0 → 1

```
55 ff 06 01 00 00 0e 99 01 00 10 04 09 08 19 0e 2e 63 00 aa fc 2f de a7
```

## DataNotExpReply / service 15 — 1 → 0

```
55 ff 06 00 01 00 12 7d 01 00 10 04 09 08 19 0f 2e 65 06 01 00 aa fc 02 00 2f 11 87
```

## DataNotExpReply / service 13 — 0 → 2

```
55 ff 06 02 00 00 44 c6 01 00 10 04 09 08 19 0d 2e 65 38 00 00 00 00 00 00 35 2e 34 00 00 00 00 00 32 31 2e 36 00 00 00 00 32 30 2e 30 00 00 00 00 00 00 00 00 00 00 00 00 01 01 00 00 00 01 00 00 00 00 00 ac fc 00 00 01 01 02 2f 2b d1
```

## DataNotExpReply / service 12 — 1 → 0

```
55 ff 06 00 01 00 39 64 01 00 10 04 09 08 19 0c 2e 65 2d 00 01 02 01 00 8b 08 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 2f 20 16
```

## DataNotExpReply / service 12 (key press) — 1 → 0

```
55 ff 06 00 01 00 39 64 01 00 10 04 09 08 19 0c 2e 65 2d 00 01 02 00 00 9d 08 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 08 00 04 00 00 00 2f 30 71
```

