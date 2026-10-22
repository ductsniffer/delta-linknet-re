// Did we see every frame? Two independent answers, both from the traffic
// itself, so an endurance run can claim "no frame lost" with evidence:
//
//  1. The token ring. Every frame must come from the station holding the
//     token, or from the station it just addressed a request to (a Poll For
//     Master reply, for instance). Missing one Token frame makes the next
//     frame come from an unexpected source: that is a ring break.
//
//     One exception is legitimate MS/TP: a station that passed the token
//     and hears nothing from the next one within its Tusage_timeout passes
//     it again (ASHRAE 135 clause 9.5.6, Nretry_token). The capture holds
//     four, all Token 0->1 sent twice, three of them right after a service
//     13 or 14 to thermostat 1. They are counted as token retries, not
//     breaks.
//
//  2. The controller's sequence counter. Services 13 and 14 share one
//     16-bit counter that goes up by one per message, whichever zone it is
//     for (docs/report.md section 4). A step other than +1 means messages missed.
//
// It also matches each acknowledgement (service 15) to the message it
// answers, which counts the 13s and 14s a thermostat never acknowledged.
#pragma once

#include <cstdint>

#include "DeltaPrivate.h"
#include "Mstp.h"

namespace linknet {

struct BusCounts {
    uint32_t frames = 0;
    uint32_t byType[9] = {};     // 0..7, then 8 for anything else
    uint32_t delta = 0;          // data frames that parse as a Delta message
    uint32_t otherData = 0;      // data frames that do not
    uint32_t ringBreaks = 0;
    uint32_t tokenRetries = 0;   // the passer sent its token again, see above
    uint32_t seqJumps = 0;      // times the counter did not step by one
    uint32_t seqMissing = 0;     // messages those jumps skipped over
    uint32_t acked = 0;
    uint32_t unacked = 0;        // a new 13/14 went out before the last was acked
    uint32_t ackMismatch = 0;    // a 15 whose sequence number answers nothing pending
};

class BusCheck {
public:
    // Every frame whose header CRC is valid, data CRC failures included: the
    // header alone says who sent it, which is all the ring check needs.
    // `m` is the Delta message in the frame, or nullptr.
    void onFrame(const Frame &f, const DeltaMessage *m);

    const BusCounts &counts() const { return c_; }
    // Whether the frame last passed to onFrame() was a token retry.
    bool lastWasRetry() const { return lastWasRetry_; }

private:
    int16_t  holder_ = -1;     // station holding the token; -1 = not known yet
    int16_t  responder_ = -1;  // station a request was just addressed to
    int16_t  passer_ = -1;     // sender of the previous frame, when it was a Token
    bool     lastWasRetry_ = false;
    bool     haveSeq_ = false;
    uint16_t lastSeq_ = 0;
    // Per zone 1..2: the sequence number of the 13/14 awaiting its 15.
    bool     pending_[3] = {};
    uint16_t pendingSeq_[3] = {};
    BusCounts c_;
};

}  // namespace linknet
