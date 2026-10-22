#include "DeltaPrivate.h"

#include <cstring>

namespace linknet {

namespace {

// NPDU version 1 without control bits; UnconfirmedRequest, service
// UnconfirmedPrivateTransfer; context tag 0 = vendor 8; context tag 1, one
// byte long, announcing the service number. tools/usb/list_messages.py HEADER, plus 19.
constexpr uint8_t kEnvelope[] = {0x01, 0x00, 0x10, 0x04, 0x09, 0x08, 0x19};
constexpr uint8_t kOpenTag2 = 0x2e;
constexpr uint8_t kCloseTag2 = 0x2f;
constexpr uint8_t kAppTagOctetString = 6;

uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void copyText(const uint8_t *p, Text &out) {
    size_t n = 0;
    while (n < kTextField && p[n] != 0) {
        out[n] = static_cast<char>(p[n]);
        ++n;
    }
    out[n] = '\0';
}

constexpr uint64_t bit(unsigned i) { return uint64_t{1} << i; }
constexpr uint64_t bits(unsigned from, unsigned to) {
    return (bit(to + 1) - 1) & ~(bit(from) - 1);
}

struct Usual {
    uint8_t index;
    uint8_t value;
};

// Marks every byte that differs from what phase 1 always saw there.
// `variable` lists the bytes expected to change; `usual` gives the constant
// value of the others where it is not 00.
uint64_t unexpectedBytes(const uint8_t *p, size_t len, uint64_t variable,
                         const Usual *usual, size_t nUsual) {
    uint64_t mask = 0;
    for (size_t i = 0; i < len && i < 64; ++i) {
        if (variable & bit(static_cast<unsigned>(i))) continue;
        uint8_t expected = 0;
        for (size_t k = 0; k < nUsual; ++k) {
            if (usual[k].index == i) expected = usual[k].value;
        }
        if (p[i] != expected) mask |= bit(static_cast<unsigned>(i));
    }
    return mask;
}

}  // namespace

bool parseDelta(const uint8_t *data, size_t len, DeltaMessage &out) {
    constexpr size_t kHead = sizeof kEnvelope;
    // envelope, service number, opening tag, tag byte, closing tag
    if (len < kHead + 4) return false;
    if (std::memcmp(data, kEnvelope, kHead) != 0) return false;
    size_t i = kHead;
    const uint8_t service = data[i++];
    if (data[i++] != kOpenTag2) return false;

    const uint8_t tag = data[i++];
    if ((tag >> 4) != kAppTagOctetString || (tag & 0x08) != 0) return false;
    size_t strLen = tag & 0x07;
    if (strLen == 5) {
        if (i >= len) return false;
        strLen = data[i++];
        if (strLen == 254) {
            if (i + 2 > len) return false;
            strLen = static_cast<size_t>((data[i] << 8) | data[i + 1]);
            i += 2;
        } else if (strLen == 255) {
            return false;  // a 4-byte length cannot fit in an MS/TP frame
        }
    } else if (strLen > 5) {
        return false;  // 6 and 7 would be opening/closing tags, not lengths
    }
    // The string, then the closing tag as the very last byte.
    if (i + strLen + 1 != len || data[len - 1] != kCloseTag2) return false;

    out.service = service;
    out.payload = data + i;
    out.len = static_cast<uint16_t>(strLen);
    return true;
}

Deci parseDeci(const char *text) {
    Deci d;
    const char *p = text;
    bool negative = false;
    if (*p == '-') {
        negative = true;
        ++p;
    }
    int whole = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p - '0');
        if (++digits > 3) return d;
        ++p;
    }
    if (digits == 0) return d;
    int tenth = 0;
    if (*p == '.') {
        ++p;
        if (*p < '0' || *p > '9') return d;
        tenth = *p - '0';
        ++p;
    }
    if (*p != '\0') return d;
    const int value = whole * 10 + tenth;
    d.tenths = static_cast<int16_t>(negative ? -value : value);
    d.valid = true;
    return d;
}

const char *keyName(Key k) {
    switch (k) {
        case Key::None: return "NONE";
        case Key::Off:  return "OFF";
        case Key::On:   return "ON";
        case Key::Down: return "DOWN";
        case Key::Up:   return "UP";
    }
    return "?";
}

bool decodeSensor(const DeltaMessage &m, SensorReport &out) {
    if (m.service != static_cast<uint8_t>(Service::Sensor) || m.len != kSensorLen) return false;
    const uint8_t *p = m.payload;
    out.zone = p[1];
    out.keyPress = p[3] == 0x00;
    out.raw = le16(p + 5);
    out.keyMask = p[39];
    out.key = p[41] <= 4 ? static_cast<Key>(p[41]) : Key::None;
    static const Usual kUsual[] = {{2, 0x02}, {3, 0x01}};
    uint64_t variable = bit(1) | bit(5) | bit(6) | bit(39) | bit(41);
    if (out.keyPress) variable |= bit(3);  // 00 is the other legitimate value
    out.unexpected = unexpectedBytes(p, m.len, variable, kUsual, 2);
    if (p[41] > 4) out.unexpected |= bit(41);
    return true;
}

bool decodeDisplay(const DeltaMessage &m, DisplayUpdate &out) {
    if (m.service != static_cast<uint8_t>(Service::Display) || m.len != kDisplayLen) return false;
    const uint8_t *p = m.payload;
    copyText(p + 6, out.outdoorText);
    copyText(p + 14, out.indoorText);
    copyText(p + 22, out.setpointText);
    out.outdoor = parseDeci(out.outdoorText);
    out.indoor = parseDeci(out.indoorText);
    out.setpoint = parseDeci(out.setpointText);
    out.fan = p[39] != 0;
    out.on = p[43] != 0;
    out.adjusting = p[45] == 0xff;
    out.seq = le16(p + 49);
    static const Usual kUsual[] = {{38, 0x01}, {53, 0x01}, {54, 0x01}, {55, 0x02}};
    const uint64_t variable = bits(6, 29) | bit(39) | bit(43) | bit(45) | bit(49) | bit(50);
    out.unexpected = unexpectedBytes(p, m.len, variable, kUsual, 4);
    // The icons are flags and byte 45 has two values: anything else is news.
    if (p[39] > 1) out.unexpected |= bit(39);
    if (p[43] > 1) out.unexpected |= bit(43);
    if (p[45] != 0x00 && p[45] != 0xff) out.unexpected |= bit(45);
    return true;
}

bool decodePoll(const DeltaMessage &m, PollRequest &out) {
    if (m.service != static_cast<uint8_t>(Service::Poll) || m.len != kPollLen) return false;
    out.seq = le16(m.payload + 1);
    return true;
}

bool decodeAck(const DeltaMessage &m, AckReport &out) {
    if (m.service != static_cast<uint8_t>(Service::Ack) || m.len != kAckLen) return false;
    out.zone = m.payload[0];
    out.seq = le16(m.payload + 2);
    out.status = m.payload[4];
    return true;
}

}  // namespace linknet
