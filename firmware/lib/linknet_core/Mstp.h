// MS/TP framing: from a stream of timed bytes to checked frames.
//
// The algorithm is deliberately the one of tools/usb/mstp.py (phase 1), so that a
// replay of the capture gives the same frames at the same offsets as the
// reference files in captures/:
//   - hunt for 55 FF;
//   - check the header CRC-8; on failure, resume the hunt at the byte after
//     the 55, so a real preamble hidden in a corrupted header is not lost;
//   - read the big-endian data length, then the data and its CRC-16;
//   - a frame whose data CRC fails is still a frame (it is counted and
//     reported with dataCrcOk = false), and the hunt resumes after it.
//
// Silence on the line is NOT used to delimit frames, unlike the MS/TP
// receive state machine (Tframe_abort): the CRCs already reject a truncated
// frame, and this keeps the replay of a timestamp-less .bin exact.
#pragma once

#include <cstddef>
#include <cstdint>

#include "Ports.h"

namespace linknet {

enum class FrameType : uint8_t {
    Token                 = 0,
    PollForMaster         = 1,
    ReplyToPollForMaster  = 2,
    TestRequest           = 3,
    TestResponse          = 4,
    DataExpectingReply    = 5,
    DataNotExpectingReply = 6,
    ReplyPostponed        = 7,
};

// The names tools/usb/mstp.py uses, so the summary lines match expected_summary.txt.
// nullptr for a type outside 0..7.
const char *frameTypeName(uint8_t type);

// ASHRAE 135 clause 9.1: the longest data field of a non-extended frame.
// A header that passes its CRC but announces more is treated as a corrupted
// header, so one bad length cannot swallow kilobytes of good frames.
constexpr uint16_t kMaxDataLen = 501;

struct Frame {
    uint8_t  type = 0;
    uint8_t  dst = 0;
    uint8_t  src = 0;
    uint16_t len = 0;           // data length; 0 for Token, Poll For Master...
    bool     dataCrcOk = true;  // true when there is no data
    uint32_t offset = 0;        // stream index of the 55 of the preamble
    uint64_t firstByteUs = 0;   // TimedByte::tUs of that 55
    uint64_t lastByteUs = 0;    // TimedByte::tUs of the last byte of the frame
    uint32_t maxByteGapUs = 0;  // largest tUs step between two of its bytes
    uint8_t  data[kMaxDataLen] = {};

    uint32_t wireLength() const { return 8u + (len ? len + 2u : 0u); }
};

struct DecoderStats {
    uint32_t bytes = 0;            // every byte pushed
    uint32_t frames = 0;           // header CRC valid (the reference "trames")
    uint32_t bytesInFrames = 0;
    uint32_t bytesPending = 0;     // the frame being received right now, so far
    uint32_t headerCrcErrors = 0;  // 55 FF followed by a header that fails its CRC
    uint32_t badLength = 0;        // valid header CRC, length above kMaxDataLen
    uint32_t dataCrcErrors = 0;

    // Bytes that belonged to no frame: noise, or the tail of a frame cut by
    // the start of the recording.
    uint32_t bytesOutsideFrames() const { return bytes - bytesInFrames - bytesPending; }
};

class FrameDecoder {
public:
    // Feeds one byte. Returns true when it completed a frame, which then
    // stays readable through frame() until the next push().
    bool push(uint8_t value, uint64_t tUs);
    bool push(const TimedByte &b) { return push(b.value, b.tUs); }

    const Frame &frame() const { return frame_; }
    const DecoderStats &stats() const;

private:
    enum class State : uint8_t { Hunt, Preamble, Header, Data };

    // One byte through the state machine. A failed header replays its six
    // bytes through here; that can never complete a frame (it would take
    // eight), so step() is not re-entered from a replay.
    bool step(uint8_t value, uint64_t tUs, uint32_t offset);
    void replayHeader();
    void noteByte(uint64_t tUs);

    State    state_ = State::Hunt;
    uint32_t nextOffset_ = 0;
    uint8_t  header_[6] = {};
    uint64_t headerUs_[6] = {};
    uint8_t  headerFill_ = 0;
    uint16_t dataFill_ = 0;   // data bytes then the two CRC bytes
    uint8_t  crcLo_ = 0;
    uint64_t prevByteUs_ = 0;
    Frame    frame_;
    mutable DecoderStats stats_;  // bytesPending is filled in by stats()
};

}  // namespace linknet
