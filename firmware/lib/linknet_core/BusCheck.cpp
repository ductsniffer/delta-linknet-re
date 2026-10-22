#include "BusCheck.h"

#include "Zones.h"

namespace linknet {

namespace {

bool isRequest(uint8_t type) {
    return type == static_cast<uint8_t>(FrameType::PollForMaster) ||
           type == static_cast<uint8_t>(FrameType::TestRequest) ||
           type == static_cast<uint8_t>(FrameType::DataExpectingReply);
}

}  // namespace

void BusCheck::onFrame(const Frame &f, const DeltaMessage *m) {
    ++c_.frames;
    ++c_.byType[f.type < 8 ? f.type : 8];

    // --- token ring --------------------------------------------------
    // Right after a Token, its sender may speak again if the receiver stayed
    // silent: a retry of the same token, or a Poll For Master to find
    // another successor. Either way the token never left the sender.
    lastWasRetry_ = f.src == passer_ && f.src != holder_;
    if (lastWasRetry_) {
        ++c_.tokenRetries;
        holder_ = f.src;
    }
    if (holder_ >= 0 && f.src != holder_ && f.src != responder_) {
        ++c_.ringBreaks;
        holder_ = f.src;  // resynchronise on whoever is talking
    }
    if (holder_ < 0) holder_ = f.src;
    passer_ = f.type == static_cast<uint8_t>(FrameType::Token) ? f.src : -1;
    if (f.type == static_cast<uint8_t>(FrameType::Token)) {
        holder_ = f.dst;
        responder_ = -1;
    } else if (isRequest(f.type)) {
        responder_ = f.dst;
    } else {
        responder_ = -1;
    }

    // --- application level -------------------------------------------
    if (f.len == 0 || !f.dataCrcOk) return;
    if (m == nullptr) {
        ++c_.otherData;
        return;
    }
    ++c_.delta;

    uint16_t seq = 0;
    bool fromController = false;
    if (m->service == static_cast<uint8_t>(Service::Display)) {
        DisplayUpdate d;
        if (decodeDisplay(*m, d)) {
            seq = d.seq;
            fromController = true;
        }
    } else if (m->service == static_cast<uint8_t>(Service::Poll)) {
        PollRequest p;
        if (decodePoll(*m, p)) {
            seq = p.seq;
            fromController = true;
        }
    }
    if (fromController && f.src == kControllerAddress) {
        if (haveSeq_ && seq != static_cast<uint16_t>(lastSeq_ + 1)) {
            ++c_.seqJumps;
            c_.seqMissing += static_cast<uint16_t>(seq - lastSeq_ - 1);
        }
        haveSeq_ = true;
        lastSeq_ = seq;
        if (f.dst >= 1 && f.dst <= kZoneCount) {
            if (pending_[f.dst]) ++c_.unacked;
            pending_[f.dst] = true;
            pendingSeq_[f.dst] = seq;
        }
        return;
    }

    AckReport a;
    if (decodeAck(*m, a) && f.dst == kControllerAddress && f.src >= 1 && f.src <= kZoneCount) {
        if (pending_[f.src] && pendingSeq_[f.src] == a.seq) {
            ++c_.acked;
            pending_[f.src] = false;
        } else {
            ++c_.ackMismatch;
        }
    }
}

}  // namespace linknet
