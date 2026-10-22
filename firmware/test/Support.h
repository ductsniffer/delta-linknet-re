// Helpers shared by the native tests: finding the project's files, reading
// hex, and building MS/TP frames. Header-only, included as "../Support.h".
#pragma once

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Crc.h"
#include "Ports.h"

namespace support {

inline bool exists(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

// The project root, the folder holding captures/ and docs/example-frames.md.
// Found from this file's own path first, so it does not depend on where pio
// starts the test binary; the container mounts the project at /work.
inline std::string root() {
    std::string self = __FILE__;
    const size_t at = self.rfind("/firmware/test/");
    std::vector<std::string> candidates;
    if (at != std::string::npos) candidates.push_back(self.substr(0, at));
    candidates.push_back("..");
    candidates.push_back("/work");
    for (const std::string &c : candidates) {
        if (exists(c + "/captures/phase1-usb/expected_summary.txt")) return c;
    }
    return "..";
}

inline std::string path(const std::string &relative) { return root() + "/" + relative; }

inline std::vector<uint8_t> readBinary(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

inline std::string readText(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Splits on \n and drops a trailing \r, so a file saved with Windows line
// endings reads the same.
inline std::vector<std::string> lines(const std::string &text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

inline std::vector<uint8_t> hex(const std::string &s) {
    std::vector<uint8_t> out;
    int nibble = -1;
    for (char c : s) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else continue;
        if (nibble < 0) {
            nibble = v;
        } else {
            out.push_back(static_cast<uint8_t>(nibble << 4 | v));
            nibble = -1;
        }
    }
    return out;
}

inline std::string toHex(const uint8_t *p, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; ++i) {
        snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}

// A complete frame, preamble and both CRCs included.
inline std::vector<uint8_t> frame(uint8_t type, uint8_t dst, uint8_t src,
                                  const std::vector<uint8_t> &data = {}) {
    std::vector<uint8_t> f = {0x55, 0xFF, type, dst, src,
                              static_cast<uint8_t>(data.size() >> 8),
                              static_cast<uint8_t>(data.size() & 0xFF)};
    f.push_back(linknet::headerCrc(f.data() + 2, 5));
    if (!data.empty()) {
        f.insert(f.end(), data.begin(), data.end());
        const uint16_t crc = linknet::dataCrc(data.data(), data.size());
        f.push_back(static_cast<uint8_t>(crc & 0xFF));
        f.push_back(static_cast<uint8_t>(crc >> 8));
    }
    return f;
}

// A Delta UnconfirmedPrivateTransfer carrying `payload` as service `sn`.
inline std::vector<uint8_t> delta(uint8_t sn, const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> d = {0x01, 0x00, 0x10, 0x04, 0x09, 0x08, 0x19, sn, 0x2e};
    if (payload.size() <= 4) {
        d.push_back(static_cast<uint8_t>(0x60 | payload.size()));
    } else {
        d.push_back(0x65);
        d.push_back(static_cast<uint8_t>(payload.size()));
    }
    d.insert(d.end(), payload.begin(), payload.end());
    d.push_back(0x2f);
    return d;
}

// Bytes on a 76 800 baud line sent back to back: one stamp per character
// time (130 us), the first at t0.
struct Wire {
    uint64_t t = 1000000;
    std::vector<linknet::TimedByte> out;

    void send(const std::vector<uint8_t> &bytes, uint32_t charUs = 130) {
        for (uint8_t b : bytes) {
            t += charUs;
            out.push_back(linknet::TimedByte{t, b});
        }
    }
    void idle(uint32_t us) { t += us; }
};

// A byte source over a prepared vector, handing out at most `chunk` bytes
// per read, like the UART ring would.
class VectorSource : public linknet::IByteSource {
public:
    explicit VectorSource(const std::vector<linknet::TimedByte> &v, size_t chunk = 64)
        : v_(v), chunk_(chunk) {}
    size_t read(linknet::TimedByte *dst, size_t cap) override {
        size_t n = 0;
        while (n < cap && n < chunk_ && pos_ < v_.size()) dst[n++] = v_[pos_++];
        return n;
    }
    linknet::RxHealth health() const override { return rx; }
    linknet::RxHealth rx;

private:
    const std::vector<linknet::TimedByte> &v_;
    size_t chunk_;
    size_t pos_ = 0;
};

class CaptureLog : public linknet::ILog {
public:
    void write(const char *line) override { lines.emplace_back(line); }
    bool any(const std::string &needle) const {
        for (const std::string &l : lines) {
            if (l.find(needle) != std::string::npos) return true;
        }
        return false;
    }
    size_t count(const std::string &needle) const {
        size_t n = 0;
        for (const std::string &l : lines) n += l.find(needle) != std::string::npos;
        return n;
    }
    std::vector<std::string> lines;
};

}  // namespace support
