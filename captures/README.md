# Captures

Reference recordings of the bus. New recordings land in this folder too, but git ignores them: only the two
sub-folders below are committed.

## `phase1-usb/`: USB RS-485 adapter, 2026-10-16

About 28 minutes of bus with scripted tests on both thermostats, recorded by `tools/usb/capture.py`.

| File | Content |
|---|---|
| `20261016-211923.bin` | every byte, in order, nothing else |
| `20261016-211923.log` | one line per serial read: `<unix time> <offset in .bin> <n> <hex bytes>`; markers as `<unix time> # MARQUEUR <text>` |
| `markers.csv` | the test markers, with their offset in the `.bin` |
| `expected_summary.txt` | byte, frame and CRC counts, per frame type, source and destination |
| `expected_delta_messages.csv` | every Delta private message: offset, time, source, destination, service, octet string |
| `calibration.csv` | 50 pairs of raw sensor value (service 12) and displayed temperature (service 13) |

The markers were typed in French during the session: `DEBUT` = start, `FIN` = end, `T1`/`T2` = thermostat 1/2,
`HAUT`/`BAS` = ▲/▼, `csg` = setpoint, `int`/`ext` = indoor/outdoor, `remise` = back to the reference state.

Timestamps have about 16 ms resolution, the latency timer of the adapter's FTDI chip.

`expected_delta_messages.csv` was produced by an early version of the listing script that dropped the leading
`00` of the service 14 octet strings. The firmware's replay test (`firmware/test/test_replay/`) knows about it.

## `phase2-esp32/`: ESP32 listener, 2026-10-21

| File | Content |
|---|---|
| `esp32-20261021-sample-raw-001.raw` | 10 minutes of bus (10:15 to 10:25) with a time in µs on every byte, including both setpoints being raised to 21.0 |
| `esp32-20261021-sample.log` | the firmware's console over the same 10 minutes: zone changes, key presses, statistics |
| `summary-24h.txt` | the analysis of the full 24-hour recording (13.5 M frames) that the report is based on |

Run the analysis on the sample:

```
./scripts/analyze.ps1 20261021-sample --captures captures/phase2-esp32 --log captures/phase2-esp32/esp32-20261021-sample.log
```

### Raw stream format (`.raw`)

Written by `scripts/raw.ps1` from the board's TCP port 24 (`firmware/lib/linknet_core/RawCapture.h`):

```
HOST <host wall time when connected, ISO 8601 with offset>\n
LNKRAW1 image=<firmware> build=<date> now_us=<board clock, us> record=6 pending=<n> lost=<n>\n
then 6-byte records: u32 LE time (us, low 32 bits of the board clock), u8 byte, u8 flags
```

- The time is the end of the byte's stop bit. The receive interrupt stamps every byte (`firmware/lib/linknet_esp32/Esp32RxUart.h`).
- Flag bit 0 means that bytes were lost just before this one, because the board's buffer was full. Nothing is measured across such a gap.
- The two header lines tie the board clock to the host clock, give or take the Wi-Fi latency.

### Console log format (`.log`)

`scripts/console.ps1` writes `<host time> <board uptime, s> <line>`. The lines that matter:

```
Z1 bedroom in=20.2 sp=20.0 out=8.1 fan=0 on=0 adj=0 raw=2247 (sp adj)   a zone changed; (...) says what
Z2 living key=UP mask=08 raw=2252                                        a key press (service 12)
STATS frames=... hdr_crc=0 data_crc=0 token_retries=... seq_missing=0    every 60 s
GAP token>1 n=... min=... p50=... p99=... max=... avg=... us             inter-frame timing, every 60 s
RETRY Token 0->1, 20060 us after the unanswered token
```
