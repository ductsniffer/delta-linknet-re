// The two MS/TP checksums, as ASHRAE 135 Annex G defines them and as
// tools/usb/mstp.py (phase 1) computes them. Both are verified against every frame of
// the 2026-10-16 capture by test_replay.
#pragma once

#include <cstddef>
#include <cstdint>

namespace linknet {

// CRC-8 over the five header bytes (type, destination, source, length hi,
// length lo). The sixth header byte on the wire is this value.
uint8_t headerCrc(const uint8_t *p, size_t n);

// CRC-16 over the data field. Sent low byte first, right after the data.
uint16_t dataCrc(const uint8_t *p, size_t n);

}  // namespace linknet
