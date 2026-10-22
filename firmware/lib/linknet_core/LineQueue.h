// Console lines waiting for the network console (TCP port 23).
//
// The ESP32 sits by a thermostat with only a charger on its USB port, so the
// console the tests read is the one over Wi-Fi. Lines are queued here as
// they are logged and sent when the network can take them: a Wi-Fi hiccup
// or a laptop reconnecting late then delays lines instead of losing them,
// and the boot banner (pin check included) is still there for a client that
// connects after boot.
//
// Whole lines in, bytes out. A line that does not fit is dropped whole, never
// cut, and the next line that fits is preceded by a notice saying how many
// went missing, so a gap in the log is always visible in the log itself.
//
// Not thread-safe: the owner (esp32::ConsoleLog) holds a mutex around it.
// No allocation: the storage is the caller's.
#pragma once

#include <cstddef>
#include <cstdint>

namespace linknet {

class LineQueue {
public:
    LineQueue(char *storage, size_t capacity) : buf_(storage), cap_(capacity) {}

    // Appends `line` and a '\n', or drops the line and counts it.
    bool push(const char *line);

    // Copies up to `cap` of the oldest bytes without removing them: the
    // network may take fewer than offered, and consume() then removes only
    // what it took.
    size_t peek(char *dst, size_t cap) const;
    void   consume(size_t n);

    size_t   size() const { return size_; }
    size_t   capacity() const { return cap_; }
    uint32_t droppedLines() const { return dropped_; }

private:
    void append(const char *s, size_t n);

    char    *buf_;
    size_t   cap_;
    size_t   head_ = 0;  // oldest byte
    size_t   size_ = 0;
    uint32_t dropped_ = 0;      // since boot
    uint32_t unreported_ = 0;   // dropped since the last notice
};

}  // namespace linknet
