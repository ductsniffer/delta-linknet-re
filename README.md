# Delta LINKnet reverse engineering

What travels on the wire between a Delta Controls HVAC controller and its **DNS-24L** wall thermostats, worked out by
**listening only**, plus the tools to listen yourself: Python scripts for a USB RS-485 adapter, and an ESP32-S3 firmware
that decodes the bus and stamps every byte in microseconds.

> **Receive-only.** Nothing in this repository transmits on the bus, and the hardware described makes it impossible.
> Read [docs/hardware.md](docs/hardware.md) before wiring anything to a live system.

## Findings in brief

- **LINKnet is standard BACnet MS/TP** at 76 800 baud, 8N1. The controller (MAC 0) and the thermostats (MAC 1 and 2) are
  all masters, passing the token 0 → 1 → 2 → 0 about 42 times a second.
- The thermostats and the controller talk through four **Delta private services** (BACnet *UnconfirmedPrivateTransfer*,
  vendor 8):

  | Service | Direction | Content |
  |---|---|---|
  | 12 | thermostat → controller | raw sensor reading, or a key press (▲ ▼ ON OFF) |
  | 13 | controller → thermostat | what to display: outdoor, indoor and setpoint **as ASCII text**, plus icons |
  | 14 | controller → thermostat | poll, every 8.5 s |
  | 15 | thermostat → controller | acknowledgement of 13 and 14 |

- **The thermostat is a remote display and keypad.** The setpoint lives in the controller. A key press goes up as a
  service 12, and the new setpoint comes back as text in service 13 within half a second.
- The controller also broadcasts a standard BACnet **TimeSynchronization** every hour.
- Timing measured over 24 hours (13.5 M frames, no CRC error):
  - the controller answers in 1.2 ms, the thermostats in 2.3 ms;
  - a token is retried after 19 to 21 ms;
  - a full token round takes 9 to 30 ms.

The full write-up, with byte maps, timing tables and plots: **[docs/report.md](docs/report.md)**.

![24 hours of both zones](docs/images/zones.png)

## Repository layout

```
docs/
  report.md            the reverse-engineering report
  hardware.md          parts, wiring, safety rules, photos
  example-frames.md    one complete frame of each kind (also the unit tests' input)
captures/              reference recordings and their formats (captures/README.md)
  phase1-usb/          28 min from a USB RS-485 adapter, with the test markers
  phase2-esp32/        10 min from the ESP32 (µs timestamps), and the 24-hour summary
tools/
  usb/                 capture.py, mark.py, list_messages.py, to_pcap.py, raw_stats.py, mstp.py
  analysis/            linknet_analysis.py: ESP32 recording -> pcap, CSV, statistics, plots
firmware/              ESP32-S3 listener (PlatformIO, Arduino framework, C++17)
  lib/linknet_core/    MS/TP framing and Delta services, no hardware dependency, unit-tested
  lib/linknet_esp32/   receive-only UART with µs stamps, Wi-Fi console, OTA, raw stream
  src/                 the listener; edges/ (wiring check); provision/ (Wi-Fi settings)
  test/                Unity tests, including a replay of the phase 1 capture
scripts/               host-side wrappers: build, test, flash, console, raw stream, analysis
docker/                build and analysis containers
```

## Listening with a USB RS-485 adapter

Needs Python 3 and `pyserial`, and an RS-485 adapter with automatic direction wired to `NET +`, `NET −` and `GND` of a
thermostat ([hardware.md](docs/hardware.md)).

```
python3 tools/usb/capture.py                         # writes captures/<session>.bin and .log
python3 tools/usb/mark.py "START test 1: reference"  # from another terminal, while it records
python3 tools/usb/list_messages.py captures/<session>.log --no-14-15
python3 tools/usb/to_pcap.py captures/<session>.log  # then open it in Wireshark (BACnet MS/TP)
```

`capture.py` never writes to the serial port.

## Listening with the ESP32

Everything builds and runs in Docker: the host needs only Docker, plus `esptool` for the first flash. The scripts come as
PowerShell and POSIX sh pairs.

```
./scripts/test.ps1        # 84 unit tests, including the replay of captures/phase1-usb (no board needed)
./scripts/build.ps1       # firmware for the ESP32-S3
./scripts/flash.ps1 -Port COM6 -Esptool C:\path\to\esptool.exe -Target provision
./scripts/provision.ps1 -Port COM6                   # Wi-Fi settings, typed here, stored in the board only
./scripts/flash.ps1 -Port COM6 -Esptool C:\path\to\esptool.exe       # the listener
./scripts/console.ps1 -Address <board IP>            # decoded lines, tcp/23
./scripts/raw.ps1 -Address <board IP>                # every byte with its µs time, tcp/24
./scripts/analyze.ps1 <session>                      # pcap, CSV, statistics and plots
```

After the first flash, `flash.ps1 -Address <board IP>` updates the board over the Wi-Fi. A new image that never comes
back online rolls back by itself. To check the wiring without starting any UART, flash `-Target edges` first. It only counts
signal edges on GPIO 18.

## Status

The bus is fully decoded for reading. Writing, by joining the token ring as a fourth master, is analysed in the report
(sections 7, 8 and 11) but is **not** implemented here.

## Disclaimer

This is independent work, not affiliated with or endorsed by Delta Controls. Product names belong to their owners. Delta's
documentation is cited, not reproduced. Use this on equipment you are responsible for, and never transmit on a building
network without the owner's consent.

## License

[MIT](LICENSE). The license covers the code, the documentation and the captures in this repository. It does not cover
third-party documents, which are only cited.
