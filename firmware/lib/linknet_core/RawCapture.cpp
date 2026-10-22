#include "RawCapture.h"

namespace linknet {

size_t RawCapture::read(TimedByte *dst, size_t cap) {
    const size_t n = src_.read(dst, cap);
    uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t tail = tail_.load(std::memory_order_acquire);
    for (size_t i = 0; i < n; ++i) {
        if (head - tail >= kRing) {
            lost_.fetch_add(1, std::memory_order_relaxed);
            gap_ = true;
            continue;
        }
        times_[head & (kRing - 1)] = static_cast<uint32_t>(dst[i].tUs);
        values_[head & (kRing - 1)] =
            static_cast<uint16_t>(dst[i].value | (gap_ ? kLostBefore : 0) << 8);
        gap_ = false;
        ++head;
    }
    head_.store(head, std::memory_order_release);
    return n;
}

size_t RawCapture::drain(uint8_t *dst, size_t cap) {
    uint32_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_acquire);
    size_t out = 0;
    while (tail != head && out + kRecordBytes <= cap) {
        const uint32_t t = times_[tail & (kRing - 1)];
        const uint16_t v = values_[tail & (kRing - 1)];
        dst[out++] = static_cast<uint8_t>(t);
        dst[out++] = static_cast<uint8_t>(t >> 8);
        dst[out++] = static_cast<uint8_t>(t >> 16);
        dst[out++] = static_cast<uint8_t>(t >> 24);
        dst[out++] = static_cast<uint8_t>(v);
        dst[out++] = static_cast<uint8_t>(v >> 8);
        ++tail;
    }
    tail_.store(tail, std::memory_order_release);
    return out;
}

uint32_t RawCapture::pending() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
}

}  // namespace linknet
