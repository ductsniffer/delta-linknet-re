#include "Timing.h"

namespace linknet {

int GapStats::binOf(uint32_t us) {
    if (us < 16) return static_cast<int>(us);
    const int msb = 31 - __builtin_clz(us);
    return 16 + (msb - 4) * 8 + static_cast<int>((us >> (msb - 3)) & 7);
}

uint32_t GapStats::binLow(int bin) {
    if (bin < 16) return static_cast<uint32_t>(bin);
    const int msb = 4 + (bin - 16) / 8;
    const uint32_t sub = static_cast<uint32_t>((bin - 16) % 8);
    return (8u + sub) << (msb - 3);
}

uint32_t GapStats::binWidth(int bin) {
    if (bin < 16) return 1;
    return 1u << (4 + (bin - 16) / 8 - 3);
}

void GapStats::add(uint32_t us) {
    ++n_;
    sum_ += us;
    if (us < min_) min_ = us;
    if (us > max_) max_ = us;
    ++hist_[binOf(us)];
}

uint32_t GapStats::quantile(double q) const {
    if (n_ == 0) return 0;
    if (q <= 0) return min_;
    if (q >= 1) return max_;
    const uint64_t rank = static_cast<uint64_t>(q * n_ + 0.5);
    uint64_t seen = 0;
    for (int b = 0; b < kBins; ++b) {
        seen += hist_[b];
        if (seen >= rank && hist_[b] != 0) {
            uint32_t mid = binLow(b) + binWidth(b) / 2;
            if (mid < min_) mid = min_;
            if (mid > max_) mid = max_;
            return mid;
        }
    }
    return max_;
}

const char *gapName(Gap g) {
    switch (g) {
        case Gap::Any:        return "any";
        case Gap::TokenTo0:   return "token>0";
        case Gap::TokenTo1:   return "token>1";
        case Gap::TokenTo2:   return "token>2";
        case Gap::AfterData0: return "data0>";
        case Gap::AfterData1: return "data1>";
        case Gap::AfterData2: return "data2>";
        case Gap::PfmSilence: return "pfm_silence";
        case Gap::PfmReply:   return "pfm_reply";
        case Gap::TokenRetry: return "token_retry";
        case Gap::Rotation0:  return "rotation0";
        case Gap::Rotation1:  return "rotation1";
        case Gap::Rotation2:  return "rotation2";
        case Gap::Ack1:       return "ack1";
        case Gap::Ack2:       return "ack2";
        case Gap::InFrame0:   return "inframe0";
        case Gap::InFrame1:   return "inframe1";
        case Gap::InFrame2:   return "inframe2";
        case Gap::Other:      return "other";
        case Gap::kCount:     break;
    }
    return "?";
}

Timing::Timing(uint32_t baud) : charUs_((10u * 1000000u + baud / 2) / baud) {}

void Timing::add(Gap g, int64_t us) {
    // Stamps carry a few microseconds of interrupt latency, so a gap of
    // zero on the wire can come out slightly negative.
    if (us < 0) us = 0;
    if (us > INT64_C(0xFFFFFFFF)) us = INT64_C(0xFFFFFFFF);
    stats_[static_cast<int>(g)].add(static_cast<uint32_t>(us));
}

namespace {

Gap station(Gap first, uint8_t n) {
    return static_cast<Gap>(static_cast<uint8_t>(first) + n);
}

}  // namespace

void Timing::onFrame(const Frame &f, const DeltaMessage *m) {
    const uint64_t start = f.firstByteUs > charUs_ ? f.firstByteUs - charUs_ : 0;

    if (havePrev_) {
        const int64_t gap = static_cast<int64_t>(start) - static_cast<int64_t>(prevEndUs_);
        lastGapUs_ = gap < 0 ? 0 : gap > INT64_C(0xFFFFFFFF) ? UINT32_MAX : static_cast<uint32_t>(gap);
        add(Gap::Any, gap);
        Gap g = Gap::Other;
        if (prevType_ == static_cast<uint8_t>(FrameType::Token)) {
            if (f.src == prevDst_ && f.src < 3) {
                g = station(Gap::TokenTo0, f.src);
            } else if (f.src == prevSrc_) {
                g = Gap::TokenRetry;
            }
        } else if (prevType_ == static_cast<uint8_t>(FrameType::PollForMaster)) {
            if (f.type == static_cast<uint8_t>(FrameType::ReplyToPollForMaster) && f.src == prevDst_) {
                g = Gap::PfmReply;
            } else if (f.src == prevSrc_) {
                g = Gap::PfmSilence;
            }
        } else if (prevType_ == static_cast<uint8_t>(FrameType::DataExpectingReply) ||
                   prevType_ == static_cast<uint8_t>(FrameType::DataNotExpectingReply)) {
            if (f.src == prevSrc_ && f.src < 3) g = station(Gap::AfterData0, f.src);
        }
        add(g, gap);
    }

    if (f.src < 3) {
        add(station(Gap::InFrame0, f.src),
            static_cast<int64_t>(f.maxByteGapUs) - static_cast<int64_t>(charUs_));
    }

    if (f.type == static_cast<uint8_t>(FrameType::Token) && f.dst < 3) {
        if (haveTokenAt_[f.dst]) {
            add(station(Gap::Rotation0, f.dst),
                static_cast<int64_t>(start) - static_cast<int64_t>(tokenAtUs_[f.dst]));
        }
        haveTokenAt_[f.dst] = true;
        tokenAtUs_[f.dst] = start;
    }

    if (m != nullptr) {
        uint16_t seq = 0;
        bool request = false;
        DisplayUpdate d;
        PollRequest p;
        AckReport a;
        if (decodeDisplay(*m, d)) {
            seq = d.seq;
            request = true;
        } else if (decodePoll(*m, p)) {
            seq = p.seq;
            request = true;
        }
        if (request && f.src == 0 && f.dst >= 1 && f.dst <= 2) {
            ackPending_[f.dst] = true;
            ackSeq_[f.dst] = seq;
            ackFromUs_[f.dst] = f.lastByteUs;
        } else if (decodeAck(*m, a) && f.dst == 0 && f.src >= 1 && f.src <= 2) {
            if (ackPending_[f.src] && ackSeq_[f.src] == a.seq) {
                add(station(Gap::Ack1, static_cast<uint8_t>(f.src - 1)),
                    static_cast<int64_t>(start) - static_cast<int64_t>(ackFromUs_[f.src]));
                ackPending_[f.src] = false;
            }
        }
    }

    havePrev_ = true;
    prevType_ = f.type;
    prevSrc_ = f.src;
    prevDst_ = f.dst;
    prevEndUs_ = f.lastByteUs;
}

}  // namespace linknet
