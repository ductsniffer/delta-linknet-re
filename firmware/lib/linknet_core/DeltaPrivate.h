// Delta Controls private messages carried in MS/TP data frames.
//
// docs/report.md section 1: every data frame on this bus is a BACnet
// UnconfirmedPrivateTransfer from vendor 8 (Delta), whose only parameter is
// one octet string:
//
//   01 00 | 10 04 | 09 08 | 19 <SN> | 2e <tag 6 + length> <octet string> 2f
//   NPDU    UPT    vendor=8  service  opening tag 2 ... closing tag 2
//
// The four service numbers are described in docs/report.md section 4. Byte
// positions below are indexes into the octet string, as there.
#pragma once

#include <cstddef>
#include <cstdint>

namespace linknet {

enum class Service : uint8_t {
    Sensor  = 12,  // thermostat -> controller: raw sensor reading or key press
    Display = 13,  // controller -> thermostat: what to show
    Poll    = 14,  // controller -> thermostat: interrogation
    Ack     = 15,  // thermostat -> controller: acknowledges a 13 or a 14
};

struct DeltaMessage {
    uint8_t        service = 0;
    const uint8_t *payload = nullptr;  // points into the frame's data
    uint16_t       len = 0;
};

// False for anything that is not exactly the envelope above, including a
// truncated or over-long octet string.
//
// The octet string length is decoded per ASHRAE 135 clause 20.2.1: up to 4
// it sits in the tag byte itself (63 = 3 bytes), beyond that the tag byte
// says 5 and the length follows (65 38 = 56 bytes). Service 14 is the one
// message that uses the short form.
bool parseDelta(const uint8_t *data, size_t len, DeltaMessage &out);

// A temperature as the controller writes it, in tenths of a degree.
struct Deci {
    bool    valid = false;
    int16_t tenths = 0;
};

// "21.6" -> 216, "-12.5" -> -125, "5" -> 50. At most one decimal, an
// optional leading minus, nothing else. Outdoor temperatures below zero have
// not been seen yet (docs/report.md section 9); this is where they are handled.
Deci parseDeci(const char *text);

// Service 13 text fields are 8 bytes, left-aligned and zero-padded
// (docs/report.md section 9). One more byte here for the terminator.
constexpr size_t kTextField = 8;
using Text = char[kTextField + 1];

enum class Key : uint8_t { None = 0, Off = 1, On = 2, Down = 3, Up = 4 };
const char *keyName(Key k);

// Service 12, 45 bytes.
struct SensorReport {
    uint8_t  zone = 0;          // byte 1: the thermostat's own address
    bool     keyPress = false;  // byte 3: 00 = key press, 01 = periodic reading
    uint16_t raw = 0;           // bytes 5-6, little-endian; falls as it warms (NTC)
    uint8_t  keyMask = 0;       // byte 39: OFF 01, DOWN 02, ON 04, UP 08
    Key      key = Key::None;   // byte 41: OFF 1, ON 2, DOWN 3, UP 4
    // Bytes the phase 1 capture never saw at anything but their usual value
    // (byte 2 = 02, every unlisted byte = 00). A set bit flags a byte worth
    // looking at, not an error: bit i stands for byte i.
    uint64_t unexpected = 0;
};
constexpr uint16_t kSensorLen = 45;
bool decodeSensor(const DeltaMessage &m, SensorReport &out);

// Service 13, 56 bytes.
struct DisplayUpdate {
    Text     outdoorText = {};   // bytes 6-13
    Text     indoorText = {};    // bytes 14-21
    Text     setpointText = {};  // bytes 22-29
    Deci     outdoor, indoor, setpoint;
    bool     fan = false;        // byte 39: fan icon (confirmed by phase 1 test 7)
    bool     on = false;         // byte 43: the word "ON"
    bool     adjusting = false;  // byte 45: ff for 10-30 s after a key press
    uint16_t seq = 0;            // bytes 49-50, little-endian, shared with service 14
    uint64_t unexpected = 0;     // as in SensorReport; usual: 38 = 01, 53-55 = 01 01 02
};
constexpr uint16_t kDisplayLen = 56;
bool decodeDisplay(const DeltaMessage &m, DisplayUpdate &out);

// Service 14, 3 bytes: 00 <seq LE>.
struct PollRequest {
    uint16_t seq = 0;
};
constexpr uint16_t kPollLen = 3;
bool decodePoll(const DeltaMessage &m, PollRequest &out);

// Service 15, 6 bytes: <zone> 00 <seq LE> <status> 00.
// Status 02 answers a service 14, 03 a service 13.
struct AckReport {
    uint8_t  zone = 0;
    uint16_t seq = 0;
    uint8_t  status = 0;
};
constexpr uint16_t kAckLen = 6;
bool decodeAck(const DeltaMessage &m, AckReport &out);

}  // namespace linknet
