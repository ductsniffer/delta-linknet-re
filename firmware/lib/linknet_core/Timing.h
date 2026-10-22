// Delays between frames, measured in microseconds at the UART.
//
// Phase 1 could not measure these: the FTDI adapter timestamped in 16 ms
// chunks (docs/report.md section 9). They are what a future transmitting station
// has to fit into: how soon the others answer the token, how long station 2
// waits for an answer to a Poll For Master before giving up, how long a
// token rotation takes.
//
// Every TimedByte is stamped at the END of the byte (the receive interrupt
// follows the stop bit), so a frame starts one character time before its
// first stamp:
//
//   gap = (next.firstByteUs - kCharUs) - previous.lastByteUs
//
// that is, the silence on the wire between the last stop bit of one frame
// and the first start bit of the next.
#pragma once

#include <cstdint>

#include "DeltaPrivate.h"
#include "Mstp.h"

namespace linknet {

// Log-scale histogram: exact below 16 us, then 8 bins per octave (12 %
// wide) up to 2^32 us. Enough to quote a median and a 99th percentile
// without storing samples; min, max and the mean are exact.
class GapStats {
public:
    static constexpr int kBins = 16 + 28 * 8;

    void add(uint32_t us);
    uint32_t count() const { return n_; }
    uint32_t min() const { return n_ ? min_ : 0; }
    uint32_t max() const { return max_; }
    uint32_t mean() const { return n_ ? static_cast<uint32_t>(sum_ / n_) : 0; }
    // Middle of the bin holding the q-th fraction of samples, clamped to
    // [min, max]. q in [0, 1].
    uint32_t quantile(double q) const;

    static int binOf(uint32_t us);
    static uint32_t binLow(int bin);
    static uint32_t binWidth(int bin);

private:
    uint32_t n_ = 0;
    uint32_t min_ = UINT32_MAX;
    uint32_t max_ = 0;
    uint64_t sum_ = 0;
    uint32_t hist_[kBins] = {};
};

// Stations 0..2 are the controller and the two thermostats (docs/report.md
// section 2). Anything else on the bus lands in Other.
enum class Gap : uint8_t {
    Any,          // every frame to the next, whatever they are: the minimum is the turnaround
    TokenTo0,     // Token to N, then N's first frame: how fast N reacts to the token
    TokenTo1,
    TokenTo2,
    AfterData0,   // N's data frame, then N's next frame (more data, or the token)
    AfterData1,
    AfterData2,
    PfmSilence,   // Poll For Master unanswered, then the poller's next frame
    PfmReply,     // Poll For Master, then the Reply To Poll For Master
    TokenRetry,   // Token unanswered, then its sender again: the passer's Tusage_timeout
    Rotation0,    // token received by N, to the next token received by N (start to start)
    Rotation1,
    Rotation2,
    Ack1,         // service 13/14 to zone N, then zone N's service 15 with the same number
    Ack2,
    InFrame0,     // longest silence between two bytes of one frame sent by N
    InFrame1,
    InFrame2,
    Other,        // consecutive frames that fit none of the above
    kCount
};
const char *gapName(Gap g);

class Timing {
public:
    explicit Timing(uint32_t baud = 76800);

    // Every frame whose header CRC is valid, in bus order. `m` is its Delta
    // message, or nullptr.
    void onFrame(const Frame &f, const DeltaMessage *m);

    const GapStats &stats(Gap g) const { return stats_[static_cast<int>(g)]; }
    // The gap measured before the frame last passed to onFrame(), in us.
    uint32_t lastGapUs() const { return lastGapUs_; }
    uint32_t charUs() const { return charUs_; }

private:
    void add(Gap g, int64_t us);

    uint32_t charUs_;
    bool     havePrev_ = false;
    uint8_t  prevType_ = 0;
    uint8_t  prevSrc_ = 0;
    uint8_t  prevDst_ = 0;
    uint64_t prevEndUs_ = 0;
    uint32_t lastGapUs_ = 0;
    bool     haveTokenAt_[3] = {};
    uint64_t tokenAtUs_[3] = {};
    bool     ackPending_[3] = {};
    uint16_t ackSeq_[3] = {};
    uint64_t ackFromUs_[3] = {};
    GapStats stats_[static_cast<int>(Gap::kCount)];
};

}  // namespace linknet
