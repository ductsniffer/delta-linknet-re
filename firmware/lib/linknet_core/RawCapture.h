// Every byte of the bus with its time, for analysis off the board.
//
// The decoder keeps only what it needs to print; a write-up of the protocol
// needs the bytes themselves: timing histograms, token rotation, response
// delays, a replay of the session through the same decoder, a .pcap. So the
// byte source is wrapped: what the decoder reads is also queued here, and
// the network task streams it to the laptop (tcp/24, raw.ps1).
//
// Two sides, two cores: read() runs in loop() (core 1) and is the only
// writer of head_; drain() runs in the network task (core 0) and is the only
// writer of tail_. Nothing else is shared but the lost-byte counter.
//
// When the ring is full -- nobody connected, or the Wi-Fi stalled -- new
// bytes are dropped, not old ones: the stream then has a single gap, and the
// first byte after it carries kLostBefore, so the gap is in the data itself.
//
// Wire record, 6 bytes, little-endian:
//   u32 time in us (low 32 bits of the clock: it wraps every 71.6 min, and
//       the bus is never silent for that long, so the reader unwraps it)
//   u8  the byte
//   u8  flags (kLostBefore)
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "Ports.h"

#ifndef LINKNET_RAW_RING
#define LINKNET_RAW_RING 8192  // ~6 s of this bus at 1.3 kB/s (docs/report.md section 3); 48 KB
#endif

namespace linknet {

class RawCapture : public IByteSource {
public:
    static constexpr size_t  kRecordBytes = 6;
    static constexpr uint8_t kLostBefore = 0x01;

    explicit RawCapture(IByteSource &src) : src_(src) {}

    // Producer side: reads from the wrapped source and queues a copy.
    size_t   read(TimedByte *dst, size_t cap) override;
    RxHealth health() const override { return src_.health(); }

    // Consumer side: encodes and removes whole records, at most cap / 6.
    size_t drain(uint8_t *dst, size_t cap);

    uint32_t pending() const;
    uint32_t lostBytes() const { return lost_.load(std::memory_order_relaxed); }

private:
    static constexpr uint32_t kRing = LINKNET_RAW_RING;
    static_assert((kRing & (kRing - 1)) == 0, "LINKNET_RAW_RING must be a power of two");

    IByteSource &src_;
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
    std::atomic<uint32_t> lost_{0};
    bool     gap_ = false;  // producer only: the next queued byte follows a loss
    uint32_t times_[kRing] = {};
    uint16_t values_[kRing] = {};  // byte | flags << 8
};

}  // namespace linknet
