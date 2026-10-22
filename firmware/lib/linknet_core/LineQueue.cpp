#include "LineQueue.h"

#include <cstdio>
#include <cstring>

namespace linknet {

bool LineQueue::push(const char *line) {
    const size_t len = strlen(line);
    char notice[64];
    size_t noticeLen = 0;
    if (unreported_ > 0) {
        const int n = snprintf(notice, sizeof notice, "NET %lu console lines dropped here (queue full)\n",
                               static_cast<unsigned long>(unreported_));
        noticeLen = n > 0 ? static_cast<size_t>(n) : 0;
    }
    if (size_ + noticeLen + len + 1 > cap_) {
        ++dropped_;
        ++unreported_;
        return false;
    }
    if (noticeLen > 0) {
        append(notice, noticeLen);
        unreported_ = 0;
    }
    append(line, len);
    append("\n", 1);
    return true;
}

void LineQueue::append(const char *s, size_t n) {
    size_t tail = (head_ + size_) % cap_;
    for (size_t i = 0; i < n; ++i) {
        buf_[tail] = s[i];
        tail = tail + 1 == cap_ ? 0 : tail + 1;
    }
    size_ += n;
}

size_t LineQueue::peek(char *dst, size_t cap) const {
    const size_t n = cap < size_ ? cap : size_;
    // At most two pieces: up to the end of the storage, then from its start.
    const size_t first = n < cap_ - head_ ? n : cap_ - head_;
    memcpy(dst, buf_ + head_, first);
    memcpy(dst + first, buf_, n - first);
    return n;
}

void LineQueue::consume(size_t n) {
    if (n > size_) n = size_;
    head_ = (head_ + n) % cap_;
    size_ -= n;
}

}  // namespace linknet
