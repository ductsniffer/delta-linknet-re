// The raw stream: bytes pass through to the decoder untouched, come out as
// 6-byte records in order, and a loss is flagged on the byte after it.
#include <unity.h>

#include <vector>

#include "RawCapture.h"

using namespace linknet;

namespace {

struct FakeSource : IByteSource {
    std::vector<TimedByte> bytes;
    size_t at = 0;
    size_t read(TimedByte *dst, size_t cap) override {
        size_t n = 0;
        while (at < bytes.size() && n < cap) dst[n++] = bytes[at++];
        return n;
    }
    RxHealth health() const override { return RxHealth(); }
};

uint32_t le32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_bytes_pass_through_and_come_out_as_records(void) {
    FakeSource src;
    src.bytes = {{0x123456789ull, 0x55}, {0x123456800ull, 0xFF}};
    RawCapture cap(src);
    TimedByte got[8];
    TEST_ASSERT_EQUAL_UINT32(2, cap.read(got, 8));
    TEST_ASSERT_EQUAL_HEX8(0x55, got[0].value);
    TEST_ASSERT_EQUAL_UINT64(0x123456789ull, got[0].tUs);
    TEST_ASSERT_EQUAL_UINT32(2, cap.pending());

    uint8_t out[32];
    TEST_ASSERT_EQUAL_UINT32(12, cap.drain(out, sizeof out));
    TEST_ASSERT_EQUAL_HEX32(0x23456789u, le32(out));  // low 32 bits only
    TEST_ASSERT_EQUAL_HEX8(0x55, out[4]);
    TEST_ASSERT_EQUAL_HEX8(0, out[5]);
    TEST_ASSERT_EQUAL_HEX32(0x23456800u, le32(out + 6));
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[10]);
    TEST_ASSERT_EQUAL_UINT32(0, cap.pending());
}

static void test_drain_never_cuts_a_record(void) {
    FakeSource src;
    src.bytes = {{1, 1}, {2, 2}, {3, 3}};
    RawCapture cap(src);
    TimedByte got[8];
    cap.read(got, 8);
    uint8_t out[16];
    TEST_ASSERT_EQUAL_UINT32(12, cap.drain(out, sizeof out));  // room for 2.67 records
    TEST_ASSERT_EQUAL_UINT32(1, cap.pending());
    TEST_ASSERT_EQUAL_UINT32(6, cap.drain(out, sizeof out));
    TEST_ASSERT_EQUAL_HEX8(3, out[4]);
}

static void test_full_ring_drops_new_bytes_and_flags_the_next(void) {
    FakeSource src;
    const uint32_t ring = LINKNET_RAW_RING;
    for (uint32_t i = 0; i < ring + 5; ++i) src.bytes.push_back({i, static_cast<uint8_t>(i)});
    RawCapture cap(src);
    TimedByte got[256];
    size_t total = 0, n;
    while ((n = cap.read(got, 256)) > 0) total += n;
    TEST_ASSERT_EQUAL_UINT32(ring + 5, total);  // the decoder still sees all
    TEST_ASSERT_EQUAL_UINT32(5, cap.lostBytes());
    TEST_ASSERT_EQUAL_UINT32(ring, cap.pending());

    uint8_t out[6];
    TEST_ASSERT_EQUAL_UINT32(6, cap.drain(out, 6));
    TEST_ASSERT_EQUAL_UINT32(0, le32(out));
    TEST_ASSERT_EQUAL_HEX8(0, out[5]);

    src.bytes.push_back({777777, 0xAB});
    TEST_ASSERT_EQUAL_UINT32(1, cap.read(got, 256));
    while (cap.pending() > 1) cap.drain(out, 6);
    TEST_ASSERT_EQUAL_UINT32(6, cap.drain(out, 6));
    TEST_ASSERT_EQUAL_UINT32(777777, le32(out));
    TEST_ASSERT_EQUAL_HEX8(0xAB, out[4]);
    TEST_ASSERT_EQUAL_HEX8(RawCapture::kLostBefore, out[5]);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_bytes_pass_through_and_come_out_as_records);
    RUN_TEST(test_drain_never_cuts_a_record);
    RUN_TEST(test_full_ring_drops_new_bytes_and_flags_the_next);
    return UNITY_END();
}
