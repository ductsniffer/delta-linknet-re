// The seam between the decoder and the hardware.
//
// Everything in linknet_core talks only to these interfaces, which is what
// lets the native tests replay the phase 1 capture through the very code
// that runs on the ESP32. Nothing here may pull in a platform header.
//
// Note what is NOT here: there is no port to write to the bus. The decoder
// has no way to transmit, by construction, not by discipline.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

namespace linknet {

// One received byte and the moment the UART finished receiving it (the end
// of its stop bit), in microseconds of a monotonic clock. The frame timing
// in Timing.h is computed from these.
struct TimedByte {
    uint64_t tUs;
    uint8_t  value;
};

// What the receiving side can tell about its own health. All counters are
// cumulative since boot.
struct RxHealth {
    uint32_t fifoOverflows = 0;  // the UART's 128-byte FIFO filled up: bytes lost
    uint32_t ringOverflows = 0;  // the ISR -> decoder ring filled up: bytes lost
    uint32_t framingErrors = 0;  // stop bit not found: noise or wrong baud rate
    uint32_t parityErrors  = 0;  // cannot happen in 8N1, counted to prove it
    uint32_t breaks        = 0;  // line held low for longer than a character
    uint32_t interrupts    = 0;  // receive interrupts taken
    uint32_t multiByte     = 0;  // ... of which found more than one byte waiting
    uint32_t timeouts      = 0;  // ... of which were the idle-line timeout
};

// Received bytes, oldest first. Non-blocking: read() returns what has arrived
// so far, possibly nothing.
struct IByteSource {
    virtual ~IByteSource() = default;
    virtual size_t   read(TimedByte *dst, size_t cap) = 0;
    virtual RxHealth health() const = 0;
};

struct IClock {
    virtual ~IClock() = default;
    virtual uint64_t nowUs() const = 0;  // monotonic, same clock as TimedByte::tUs
};

// The console. One call per complete line, without the line ending.
struct ILog {
    virtual ~ILog() = default;
    virtual void write(const char *line) = 0;
    void logf(const char *fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
        ;
};

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb &o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Rgb &o) const { return !(*this == o); }
};

struct ILed {
    virtual ~ILed() = default;
    virtual void show(const Rgb &c) = 0;
};

}  // namespace linknet
