// The whole receive path, from timed bytes to console lines:
//
//   IByteSource -> FrameDecoder -> BusCheck, Timing   (every frame)
//                               -> Zones -> one line per zone change
//
// and, every statsPeriodS, a block of statistics. This is what runs on the
// ESP32 and what the native tests drive, minus the UART.
#pragma once

#include <cstdint>

#include "BusCheck.h"
#include "Mstp.h"
#include "Ports.h"
#include "Timing.h"
#include "Zones.h"

namespace linknet {

struct ListenerConfig {
    uint32_t baud = 76800;       // docs/report.md section 1
    uint32_t statsPeriodS = 60;  // docs/report.md section 11
    uint32_t staleS = 90;        // service 13 comes every ~30 s at the slowest (docs/report.md section 3)
    uint32_t silentMs = 1000;    // the token goes round ~42 times a second
    uint32_t errorHoldS = 60;    // how long the LED stays amber after an error
};

class Listener {
public:
    explicit Listener(ILog &log, const ListenerConfig &cfg = ListenerConfig());

    // Drains everything the source has. Lines go out as frames complete.
    void poll(IByteSource &src);
    void push(const TimedByte &b);

    // Call often. Prints the statistics block when it is due, and notes the
    // bus going silent and coming back.
    void tick(uint64_t nowUs, const RxHealth &rx);
    void printStats(uint64_t nowUs, const RxHealth &rx);

    // Blue until the first frame, then green; amber for errorHoldS after any
    // error; red while the bus is silent.
    Rgb status(uint64_t nowUs) const;

    const FrameDecoder &decoder() const { return decoder_; }
    const BusCheck &check() const { return check_; }
    const Timing &timing() const { return timing_; }
    const Zones &zones() const { return zones_; }

private:
    void onFrame(const Frame &f);
    void printZone(uint64_t tUs, uint8_t zone, const char *suffix);
    void noteErrors(uint64_t tUs);

    ILog          &log_;
    ListenerConfig cfg_;
    FrameDecoder   decoder_;
    BusCheck       check_;
    Timing         timing_;
    Zones          zones_;

    bool     haveFrame_ = false;
    uint64_t lastFrameUs_ = 0;
    bool     silent_ = false;
    uint64_t nextStatsUs_ = 0;
    bool     haveError_ = false;
    uint64_t lastErrorUs_ = 0;
    uint32_t errorTotal_ = 0;      // sum of every error counter, to spot a change
    uint32_t rxErrorTotal_ = 0;
    uint64_t oddReported_[3][2] = {};  // per zone, for services 12 and 13
};

}  // namespace linknet
