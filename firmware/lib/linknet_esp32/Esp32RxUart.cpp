#include "Esp32RxUart.h"

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_intr_alloc.h>
#include <esp_timer.h>
#include <hal/uart_ll.h>
#include <soc/uart_periph.h>

namespace linknet {
namespace esp32 {

namespace {

// Receive-side causes only. No transmit interrupt is ever enabled: there is
// nothing to transmit.
constexpr uint32_t kRxInterrupts = UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT |
                                   UART_INTR_RXFIFO_OVF | UART_INTR_FRAM_ERR |
                                   UART_INTR_PARITY_ERR | UART_INTR_BRK_DET;

// Idle-line timeout, in bit times (UART_RX_TOUT_THRHD counts bits on the
// ESP32-S3). It only matters when the per-byte interrupt was missed: the
// bytes still waiting are then stamped as having ended this long ago.
constexpr uint16_t kToutBits = 20;

}  // namespace

bool RxOnlyUart::begin(int rxPin, uint32_t baud) {
    charUs_ = (10u * 1000000u + baud / 2) / baud;
    toutUs_ = (kToutBits * 1000000u + baud / 2) / baud;

    uart_config_t cfg = {};
    cfg.baud_rate = static_cast<int>(baud);
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_APB;
    if (uart_param_config(port_, &cfg) != ESP_OK) return false;

    // Pull-up first. GPIO 18 is UART1's IO MUX pin, and on that path
    // uart_set_pin() does not set the pull; routed while still floating low
    // (it reads 0 at boot), the pad gave the UART one junk byte on the bench.
    // Never a pull-down (docs/hardware.md, safety rules).
    const gpio_num_t pin = static_cast<gpio_num_t>(rxPin);
    gpio_pulldown_dis(pin);
    gpio_pullup_en(pin);
    delayMicroseconds(100);

    // Receive pin only. TX, RTS and CTS stay UART_PIN_NO_CHANGE, so the
    // UART's transmit signal reaches no pad at all and GPIO 17 keeps its
    // reset state.
    if (uart_set_pin(port_, UART_PIN_NO_CHANGE, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) !=
        ESP_OK) {
        return false;
    }
    gpio_pulldown_dis(pin);
    gpio_pullup_en(pin);
    // Let a character caught half-way through finish before the FIFO is
    // emptied: four character times.
    delayMicroseconds(4 * charUs_);

    uart_dev_t *hw = UART_LL_GET_HW(port_);
    uart_ll_disable_intr_mask(hw, UART_LL_INTR_MASK);
    uart_ll_clr_intsts_mask(hw, UART_LL_INTR_MASK);
    uart_ll_rxfifo_rst(hw);
    uart_ll_set_rxfifo_full_thr(hw, 1);
    uart_ll_set_rx_tout(hw, kToutBits);

    // IRAM so a flash access cannot hold it off; level 3 so the console's
    // own UART interrupt and the tick cannot either.
    //
    // esp_intr_alloc() directly, not uart_isr_register(): in ESP-IDF 4.4 the
    // latter also stores the handle in the driver's state, which only
    // exists once uart_driver_install() has run, and dereferences null
    // without it (StoreProhibited at 0x8, first boot on the bench,
    // 2026-10-17). The driver is deliberately not installed here.
    if (esp_intr_alloc(uart_periph_signal[port_].irq, ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3,
                       &RxOnlyUart::isr, this, &handle_) != ESP_OK) {
        return false;
    }
    uart_ll_ena_intr_mask(hw, kRxInterrupts);
    return true;
}

void IRAM_ATTR RxOnlyUart::isr(void *arg) { static_cast<RxOnlyUart *>(arg)->onInterrupt(); }

void IRAM_ATTR RxOnlyUart::onInterrupt() {
    uart_dev_t *hw = UART_LL_GET_HW(port_);
    ++interrupts_;

    // Bounded: a stuck cause must not hang the core.
    for (int round = 0; round < 4; ++round) {
        const uint32_t status = uart_ll_get_intsts_mask(hw);
        if (status == 0) break;
        // Read per round: a byte that arrived while the last round ran is
        // younger than the interrupt.
        const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());

        uint8_t buf[128];
        uint32_t n = uart_ll_get_rxfifo_len(hw);
        if (n > sizeof buf) n = sizeof buf;
        uart_ll_read_rxfifo(hw, buf, n);

        if (status & UART_INTR_RXFIFO_OVF) {
            ++fifoOverflows_;
            uart_ll_rxfifo_rst(hw);
        }
        if (status & UART_INTR_FRAM_ERR) ++framingErrors_;
        if (status & UART_INTR_PARITY_ERR) ++parityErrors_;
        if (status & UART_INTR_BRK_DET) ++breaks_;

        if (n > 0) {
            // The last byte ended now -- or, when the idle timeout is what
            // fired, kToutBits ago. Earlier bytes were back to back.
            uint64_t end = now;
            if (status & UART_INTR_RXFIFO_TOUT) {
                ++timeouts_;
                end -= toutUs_;
            }
            if (n > 1) ++multiByte_;
            uint32_t head = head_.load(std::memory_order_relaxed);
            const uint32_t tail = tail_.load(std::memory_order_acquire);
            for (uint32_t i = 0; i < n; ++i) {
                if (head - tail >= kRing) {
                    ++ringOverflows_;
                    continue;
                }
                values_[head & (kRing - 1)] = buf[i];
                times_[head & (kRing - 1)] = end - static_cast<uint64_t>(n - 1 - i) * charUs_;
                ++head;
            }
            head_.store(head, std::memory_order_release);
        }
        uart_ll_clr_intsts_mask(hw, status);
    }
}

size_t RxOnlyUart::read(TimedByte *dst, size_t cap) {
    uint32_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_acquire);
    size_t n = 0;
    while (tail != head && n < cap) {
        dst[n].value = values_[tail & (kRing - 1)];
        dst[n].tUs = times_[tail & (kRing - 1)];
        ++tail;
        ++n;
    }
    tail_.store(tail, std::memory_order_release);
    return n;
}

RxHealth RxOnlyUart::health() const {
    RxHealth h;
    h.fifoOverflows = fifoOverflows_;
    h.ringOverflows = ringOverflows_;
    h.framingErrors = framingErrors_;
    h.parityErrors = parityErrors_;
    h.breaks = breaks_;
    h.interrupts = interrupts_;
    h.multiByte = multiByte_;
    h.timeouts = timeouts_;
    return h;
}

}  // namespace esp32
}  // namespace linknet
