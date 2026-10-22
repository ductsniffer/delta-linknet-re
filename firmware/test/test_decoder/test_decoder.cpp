// FrameDecoder corner cases the capture does not contain: corrupted
// headers, impossible lengths, preambles hidden in noise, and the
// timestamps a frame carries.
#include <unity.h>

#include <vector>

#include "../Support.h"
#include "Mstp.h"

using namespace linknet;

namespace {

std::vector<Frame> decodeAll(FrameDecoder &d, const std::vector<uint8_t> &bytes) {
    std::vector<Frame> out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (d.push(bytes[i], 1000 + i)) out.push_back(d.frame());
    }
    return out;
}

const std::vector<uint8_t> kToken12 = support::frame(0, 2, 1);

}  // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_55_55_ff_takes_the_second_55(void) {
    std::vector<uint8_t> b = {0x55};
    b.insert(b.end(), kToken12.begin(), kToken12.end());
    FrameDecoder d;
    const auto f = decodeAll(d, b);
    TEST_ASSERT_EQUAL_UINT32(1, f.size());
    TEST_ASSERT_EQUAL_UINT32(1, f[0].offset);
    TEST_ASSERT_EQUAL_UINT32(1, d.stats().bytesOutsideFrames());
}

// 55 FF, then a header whose own first bytes are a real 55 FF: the CRC
// fails on the false header and the real frame is still found.
static void test_preamble_inside_a_bad_header(void) {
    std::vector<uint8_t> b = {0x55, 0xFF};
    b.insert(b.end(), kToken12.begin(), kToken12.end());
    TEST_ASSERT_NOT_EQUAL(kToken12[5], headerCrc(kToken12.data(), 5));  // the false header fails
    FrameDecoder d;
    const auto f = decodeAll(d, b);
    TEST_ASSERT_EQUAL_UINT32(1, f.size());
    TEST_ASSERT_EQUAL_UINT32(2, f[0].offset);
    TEST_ASSERT_EQUAL_UINT8(1, f[0].src);
    TEST_ASSERT_EQUAL_UINT8(2, f[0].dst);
    TEST_ASSERT_EQUAL_UINT32(1, d.stats().headerCrcErrors);
}

static void test_length_above_501_is_rejected(void) {
    // A header with a valid CRC announcing 600 bytes, then a real token.
    std::vector<uint8_t> h = {0x55, 0xFF, 0x06, 0x01, 0x00, 0x02, 0x58};
    h.push_back(headerCrc(h.data() + 2, 5));
    h.insert(h.end(), kToken12.begin(), kToken12.end());
    FrameDecoder d;
    const auto f = decodeAll(d, h);
    TEST_ASSERT_EQUAL_UINT32(1, d.stats().badLength);
    TEST_ASSERT_EQUAL_UINT32(1, f.size());
    TEST_ASSERT_EQUAL_UINT8(0, f[0].type);
}

static void test_noise_between_frames_is_counted(void) {
    std::vector<uint8_t> b = {0x00, 0x13, 0x55, 0x00, 0xFF};
    b.insert(b.end(), kToken12.begin(), kToken12.end());
    FrameDecoder d;
    TEST_ASSERT_EQUAL_UINT32(1, decodeAll(d, b).size());
    TEST_ASSERT_EQUAL_UINT32(5, d.stats().bytesOutsideFrames());
    TEST_ASSERT_EQUAL_UINT32(0, d.stats().headerCrcErrors);
}

static void test_frame_in_progress_is_not_junk(void) {
    const std::vector<uint8_t> data = support::delta(14, {0x00, 0xaa, 0xfc});
    const std::vector<uint8_t> b = support::frame(6, 1, 0, data);
    FrameDecoder d;
    for (size_t i = 0; i + 1 < b.size(); ++i) TEST_ASSERT_FALSE(d.push(b[i], i));
    TEST_ASSERT_EQUAL_UINT32(0, d.stats().bytesOutsideFrames());
    TEST_ASSERT_EQUAL_UINT32(b.size() - 1, d.stats().bytesPending);
    TEST_ASSERT_TRUE(d.push(b.back(), 99));
    TEST_ASSERT_EQUAL_UINT32(0, d.stats().bytesPending);
    TEST_ASSERT_EQUAL_UINT32(b.size(), d.stats().bytesInFrames);
}

static void test_timestamps_of_a_frame(void) {
    support::Wire w;
    w.send(kToken12);
    w.idle(500);
    const std::vector<uint8_t> data = support::delta(14, {0x00, 0x01, 0x02});
    std::vector<uint8_t> f = support::frame(6, 1, 0, data);
    // A 300 us hole after the header: a slow sender, or interrupt jitter.
    w.send(std::vector<uint8_t>(f.begin(), f.begin() + 8));
    w.idle(300);
    w.send(std::vector<uint8_t>(f.begin() + 8, f.end()));
    FrameDecoder d;
    std::vector<Frame> got;
    for (const TimedByte &b : w.out) {
        if (d.push(b)) got.push_back(d.frame());
    }
    TEST_ASSERT_EQUAL_UINT32(2, got.size());
    TEST_ASSERT_EQUAL_UINT64(w.out[0].tUs, got[0].firstByteUs);
    TEST_ASSERT_EQUAL_UINT64(w.out[7].tUs, got[0].lastByteUs);
    TEST_ASSERT_EQUAL_UINT32(130, got[0].maxByteGapUs);
    TEST_ASSERT_EQUAL_UINT64(w.out[8].tUs, got[1].firstByteUs);
    TEST_ASSERT_EQUAL_UINT64(w.out.back().tUs, got[1].lastByteUs);
    TEST_ASSERT_EQUAL_UINT32(430, got[1].maxByteGapUs);
}

static void test_every_type_name(void) {
    TEST_ASSERT_EQUAL_STRING("ReplyToPFM", frameTypeName(2));
    TEST_ASSERT_EQUAL_STRING("DataExpReply", frameTypeName(5));
    TEST_ASSERT_EQUAL_STRING("DataNotExpReply", frameTypeName(6));
    TEST_ASSERT_EQUAL_STRING("ReplyPostponed", frameTypeName(7));
    TEST_ASSERT_NULL(frameTypeName(8));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_55_55_ff_takes_the_second_55);
    RUN_TEST(test_preamble_inside_a_bad_header);
    RUN_TEST(test_length_above_501_is_rejected);
    RUN_TEST(test_noise_between_frames_is_counted);
    RUN_TEST(test_frame_in_progress_is_not_junk);
    RUN_TEST(test_timestamps_of_a_frame);
    RUN_TEST(test_every_type_name);
    return UNITY_END();
}
