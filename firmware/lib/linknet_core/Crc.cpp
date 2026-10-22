#include "Crc.h"

namespace linknet {

// Annex G.1 gives the header CRC as a byte-wise shift-and-xor rather than a
// table; this is that formulation, transcribed from tools/usb/tools/usb/mstp.py crc8_header().
uint8_t headerCrc(const uint8_t *p, size_t n) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < n; ++i) {
        uint16_t x = static_cast<uint16_t>(crc ^ p[i]);
        x ^= static_cast<uint16_t>((x << 1) ^ (x << 2) ^ (x << 3) ^ (x << 4) ^
                                   (x << 5) ^ (x << 6) ^ (x << 7));
        crc = static_cast<uint8_t>((x & 0xFE) ^ ((x >> 8) & 1));
    }
    return static_cast<uint8_t>(~crc);
}

// CRC-CCITT, reflected (polynomial 0x8408), initial 0xFFFF, complemented.
uint16_t dataCrc(const uint8_t *p, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408)
                            : static_cast<uint16_t>(crc >> 1);
        }
    }
    return static_cast<uint16_t>(~crc);
}

}  // namespace linknet
