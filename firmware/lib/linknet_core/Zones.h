// What each thermostat shows and measures, as seen on the bus.
//
// A zone is a thermostat address. Its display state comes ONLY from the
// service 13 frames sent to that address (docs/report.md section 9), its sensor
// state from the service 12 frames it sends.
#pragma once

#include <cstdint>

#include "DeltaPrivate.h"
#include "Mstp.h"

namespace linknet {

// docs/report.md section 2: 1 = bedroom (DIP 1000), 2 = living room (DIP 0100).
constexpr uint8_t kZoneCount = 2;
constexpr uint8_t kControllerAddress = 0;
const char *zoneName(uint8_t zone);  // "bedroom", "living", or "?"

struct ZoneState {
    bool     haveDisplay = false;
    Text     outdoorText = {};
    Text     indoorText = {};
    Text     setpointText = {};
    Deci     outdoor, indoor, setpoint;
    bool     fan = false;
    bool     on = false;
    bool     adjusting = false;
    uint64_t lastDisplayUs = 0;
    uint32_t displays = 0;

    bool     haveSensor = false;
    uint16_t raw = 0;
    uint64_t lastSensorUs = 0;
    uint32_t sensors = 0;

    Key      lastKey = Key::None;
    uint32_t keys = 0;
};

// Bits of ZoneUpdate::changed.
enum ZoneField : uint16_t {
    kFieldOutdoor   = 1u << 0,
    kFieldIndoor    = 1u << 1,
    kFieldSetpoint  = 1u << 2,
    kFieldFan       = 1u << 3,
    kFieldOn        = 1u << 4,
    kFieldAdjusting = 1u << 5,
    kFieldRaw       = 1u << 6,
    kFieldFirst     = 1u << 7,  // first service 13 seen for this zone
};

struct ZoneUpdate {
    uint8_t  zone = 0;          // 0 = the frame was about no zone
    uint16_t changed = 0;       // ZoneField bits
    bool     keyPressed = false;
    Key      key = Key::None;
    uint8_t  keyMask = 0;
    // Bytes outside the known fields that differ from phase 1 (see
    // DeltaPrivate.h), for the console to print the whole payload once.
    uint64_t unexpected = 0;
    uint8_t  service = 0;
    // Service 12 announcing one zone in byte 1 while coming from another
    // address. Phase 1 never saw it; it matters for any future emission
    // (docs/report.md section 7, "main unknown").
    bool     addressMismatch = false;
};

class Zones {
public:
    // Only data frames with a valid CRC should be passed in.
    ZoneUpdate apply(const Frame &f, uint64_t nowUs);

    // zone is 1-based, as on the bus. nullptr outside 1..kZoneCount.
    const ZoneState *zone(uint8_t zone) const;

private:
    ZoneState zones_[kZoneCount];
};

}  // namespace linknet
