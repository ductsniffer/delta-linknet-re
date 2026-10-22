#include "Mstp.h"

#include "Crc.h"

namespace linknet {

const char *frameTypeName(uint8_t type) {
    // tools/usb/mstp.py TYPES, spelling included.
    static const char *const kNames[] = {
        "Token",        "PollForMaster", "ReplyToPFM",      "TestRequest",
        "TestResponse", "DataExpReply",  "DataNotExpReply", "ReplyPostponed",
    };
    return type < sizeof kNames / sizeof kNames[0] ? kNames[type] : nullptr;
}

const DecoderStats &FrameDecoder::stats() const {
    switch (state_) {
        case State::Hunt:     stats_.bytesPending = 0; break;
        case State::Preamble: stats_.bytesPending = 1; break;
        case State::Header:   stats_.bytesPending = 2u + headerFill_; break;
        case State::Data:     stats_.bytesPending = 8u + dataFill_; break;
    }
    return stats_;
}

bool FrameDecoder::push(uint8_t value, uint64_t tUs) {
    ++stats_.bytes;
    return step(value, tUs, nextOffset_++);
}

void FrameDecoder::noteByte(uint64_t tUs) {
    const uint64_t gap = tUs - prevByteUs_;
    if (gap > frame_.maxByteGapUs) {
        frame_.maxByteGapUs = gap > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(gap);
    }
    prevByteUs_ = tUs;
}

bool FrameDecoder::step(uint8_t value, uint64_t tUs, uint32_t offset) {
    switch (state_) {
        case State::Hunt:
            if (value == 0x55) {
                frame_.offset = offset;
                frame_.firstByteUs = tUs;
                frame_.maxByteGapUs = 0;
                prevByteUs_ = tUs;
                state_ = State::Preamble;
            }
            return false;

        case State::Preamble:
            if (value == 0xFF) {
                noteByte(tUs);
                headerFill_ = 0;
                state_ = State::Header;
            } else if (value == 0x55) {
                // 55 55 FF: the preamble is the second 55, as find() in
                // tools/usb/mstp.py would have it.
                frame_.offset = offset;
                frame_.firstByteUs = tUs;
                frame_.maxByteGapUs = 0;
                prevByteUs_ = tUs;
            } else {
                state_ = State::Hunt;
            }
            return false;

        case State::Header: {
            noteByte(tUs);
            header_[headerFill_] = value;
            headerUs_[headerFill_] = tUs;
            if (++headerFill_ < sizeof header_) return false;

            if (headerCrc(header_, 5) != header_[5]) {
                ++stats_.headerCrcErrors;
                replayHeader();
                return false;
            }
            const uint16_t len = static_cast<uint16_t>((header_[3] << 8) | header_[4]);
            if (len > kMaxDataLen) {
                ++stats_.badLength;
                replayHeader();
                return false;
            }
            frame_.type = header_[0];
            frame_.dst = header_[1];
            frame_.src = header_[2];
            frame_.len = len;
            frame_.dataCrcOk = true;
            if (len == 0) {
                frame_.lastByteUs = tUs;
                ++stats_.frames;
                stats_.bytesInFrames += frame_.wireLength();
                state_ = State::Hunt;
                return true;
            }
            dataFill_ = 0;
            state_ = State::Data;
            return false;
        }

        case State::Data:
            noteByte(tUs);
            if (dataFill_ < frame_.len) {
                frame_.data[dataFill_++] = value;
                return false;
            }
            if (dataFill_ == frame_.len) {
                crcLo_ = value;
                ++dataFill_;
                return false;
            }
            {
                const uint16_t received = static_cast<uint16_t>(crcLo_ | (value << 8));
                frame_.dataCrcOk = dataCrc(frame_.data, frame_.len) == received;
            }
            if (!frame_.dataCrcOk) ++stats_.dataCrcErrors;
            frame_.lastByteUs = tUs;
            ++stats_.frames;
            stats_.bytesInFrames += frame_.wireLength();
            state_ = State::Hunt;
            return true;
    }
    return false;
}

// tools/usb/mstp.py does `i += 1` after a bad header: the search restarts right after
// the 55, and since the FF that follows cannot start a preamble, that means
// at the first header byte. Re-feeding the six header bytes is the same
// search, done on a stream instead of a buffer.
void FrameDecoder::replayHeader() {
    uint8_t bytes[sizeof header_];
    uint64_t times[sizeof header_];
    for (size_t i = 0; i < sizeof header_; ++i) {
        bytes[i] = header_[i];
        times[i] = headerUs_[i];
    }
    const uint32_t first = frame_.offset + 2;
    state_ = State::Hunt;
    for (size_t i = 0; i < sizeof header_; ++i) {
        step(bytes[i], times[i], first + static_cast<uint32_t>(i));
    }
}

}  // namespace linknet
