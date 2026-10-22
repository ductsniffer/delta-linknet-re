// UART1 as a receiver and nothing else, each byte stamped in microseconds.
//
// RECEIVE ONLY. begin() routes a receive pin and no transmit pin
// (UART_PIN_NO_CHANGE), and this class has no method that writes: there is
// no uart_write_bytes() anywhere in the firmware. GPIO 17, UART1's default
// TX, is never touched.
//
// Why a hand-written interrupt rather than the ESP-IDF driver: the driver
// buffers bytes and reports them in batches, with no time on them, and the
// whole point here is the timing between frames (docs/report.md section 11). This
// handler takes an interrupt per byte (FIFO threshold 1), reads
// esp_timer_get_time() on entry, and hands (time, byte) pairs to the
// decoder through a lock-free ring. When it finds several bytes waiting --
// the interrupt was held off -- it back-dates them one character time each,
// and counts that it had to.
#pragma once

#include <Arduino.h>
#include <driver/uart.h>
#include <esp_intr_alloc.h>

#include <atomic>

#include "Ports.h"

#ifndef LINKNET_RX_RING
#define LINKNET_RX_RING 4096  // ~3 s of this bus at 1.3 kB/s (docs/report.md section 3)
#endif

namespace linknet {
namespace esp32 {

class RxOnlyUart : public IByteSource {
public:
    // Configures UART1 at `baud`, 8N1, receiving on `rxPin` (input, pull-up,
    // no pull-down), and installs the interrupt. Returns false and leaves
    // the UART without interrupt if any step fails.
    bool begin(int rxPin, uint32_t baud);

    size_t   read(TimedByte *dst, size_t cap) override;
    RxHealth health() const override;


private:
    static void isr(void *arg);
    void onInterrupt();

    static constexpr uint32_t kRing = LINKNET_RX_RING;
    static_assert((kRing & (kRing - 1)) == 0, "LINKNET_RX_RING must be a power of two");

    uart_port_t port_ = UART_NUM_1;
    uint32_t    charUs_ = 130;
    uint32_t    toutUs_ = 260;
    intr_handle_t handle_ = nullptr;

    // Written by the interrupt only: head_, values_, times_, counters.
    // Written by read() only: tail_.
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
    uint8_t  values_[kRing] = {};
    uint64_t times_[kRing] = {};
    volatile uint32_t fifoOverflows_ = 0;
    volatile uint32_t ringOverflows_ = 0;
    volatile uint32_t framingErrors_ = 0;
    volatile uint32_t parityErrors_ = 0;
    volatile uint32_t breaks_ = 0;
    volatile uint32_t interrupts_ = 0;
    volatile uint32_t multiByte_ = 0;
    volatile uint32_t timeouts_ = 0;
};

}  // namespace esp32
}  // namespace linknet
