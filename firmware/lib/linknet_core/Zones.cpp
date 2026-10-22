#include "Zones.h"

#include <cstring>

namespace linknet {

const char *zoneName(uint8_t zone) {
    switch (zone) {
        case 1: return "bedroom";
        case 2: return "living";
    }
    return "?";
}

const ZoneState *Zones::zone(uint8_t zone) const {
    if (zone < 1 || zone > kZoneCount) return nullptr;
    return &zones_[zone - 1];
}

namespace {

bool sameText(const Text &a, const Text &b) { return std::strcmp(a, b) == 0; }

}  // namespace

ZoneUpdate Zones::apply(const Frame &f, uint64_t nowUs) {
    ZoneUpdate u;
    if (f.type != static_cast<uint8_t>(FrameType::DataNotExpectingReply) || !f.dataCrcOk) return u;
    DeltaMessage m;
    if (!parseDelta(f.data, f.len, m)) return u;
    u.service = m.service;

    if (m.service == static_cast<uint8_t>(Service::Display) && f.src == kControllerAddress &&
        f.dst >= 1 && f.dst <= kZoneCount) {
        DisplayUpdate d;
        if (!decodeDisplay(m, d)) return u;
        ZoneState &z = zones_[f.dst - 1];
        u.zone = f.dst;
        u.unexpected = d.unexpected;
        if (!z.haveDisplay) u.changed |= kFieldFirst;
        if (!z.haveDisplay || !sameText(z.outdoorText, d.outdoorText)) u.changed |= kFieldOutdoor;
        if (!z.haveDisplay || !sameText(z.indoorText, d.indoorText)) u.changed |= kFieldIndoor;
        if (!z.haveDisplay || !sameText(z.setpointText, d.setpointText)) u.changed |= kFieldSetpoint;
        if (!z.haveDisplay || z.fan != d.fan) u.changed |= kFieldFan;
        if (!z.haveDisplay || z.on != d.on) u.changed |= kFieldOn;
        if (!z.haveDisplay || z.adjusting != d.adjusting) u.changed |= kFieldAdjusting;
        std::memcpy(z.outdoorText, d.outdoorText, sizeof z.outdoorText);
        std::memcpy(z.indoorText, d.indoorText, sizeof z.indoorText);
        std::memcpy(z.setpointText, d.setpointText, sizeof z.setpointText);
        z.outdoor = d.outdoor;
        z.indoor = d.indoor;
        z.setpoint = d.setpoint;
        z.fan = d.fan;
        z.on = d.on;
        z.adjusting = d.adjusting;
        z.haveDisplay = true;
        z.lastDisplayUs = nowUs;
        ++z.displays;
        return u;
    }

    if (m.service == static_cast<uint8_t>(Service::Sensor) && f.dst == kControllerAddress &&
        f.src >= 1 && f.src <= kZoneCount) {
        SensorReport s;
        if (!decodeSensor(m, s)) return u;
        ZoneState &z = zones_[f.src - 1];
        u.zone = f.src;
        u.unexpected = s.unexpected;
        u.addressMismatch = s.zone != f.src;
        if (!z.haveSensor || z.raw != s.raw) u.changed |= kFieldRaw;
        z.raw = s.raw;
        z.haveSensor = true;
        z.lastSensorUs = nowUs;
        ++z.sensors;
        if (s.keyPress) {
            u.keyPressed = true;
            u.key = s.key;
            u.keyMask = s.keyMask;
            z.lastKey = s.key;
            ++z.keys;
        }
        return u;
    }
    return u;
}

}  // namespace linknet
