// Inter-frame delays on a synthetic bus with known silences, and the
// histogram behind the percentiles.
#include <unity.h>

#include <vector>

#include "../Support.h"
#include "Timing.h"

using namespace linknet;
using support::delta;
using support::frame;

namespace {

void feed(Timing &t, const std::vector<TimedByte> &bytes) {
    FrameDecoder d;
    for (const TimedByte &b : bytes) {
        if (!d.push(b)) continue;
        const Frame &f = d.frame();
        DeltaMessage m;
        const bool isDelta = f.len && parseDelta(f.data, f.len, m);
        t.onFrame(f, isDelta ? &m : nullptr);
    }
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_bins_cover_every_value(void) {
    for (uint32_t v = 0; v < 200000; v += (v < 1000 ? 1 : 97)) {
        const int b = GapStats::binOf(v);
        TEST_ASSERT_TRUE(b >= 0 && b < GapStats::kBins);
        TEST_ASSERT_TRUE(GapStats::binLow(b) <= v);
        TEST_ASSERT_TRUE(v < GapStats::binLow(b) + GapStats::binWidth(b));
    }
    TEST_ASSERT_EQUAL_INT(GapStats::kBins - 1, GapStats::binOf(UINT32_MAX));
}

static void test_quantiles_within_a_bin(void) {
    GapStats s;
    for (uint32_t v = 1; v <= 1000; ++v) s.add(v);
    TEST_ASSERT_EQUAL_UINT32(1000, s.count());
    TEST_ASSERT_EQUAL_UINT32(1, s.min());
    TEST_ASSERT_EQUAL_UINT32(1000, s.max());
    TEST_ASSERT_EQUAL_UINT32(500, s.mean());
    TEST_ASSERT_UINT32_WITHIN(500 / 8, 500, s.quantile(0.5));
    TEST_ASSERT_UINT32_WITHIN(990 / 8, 990, s.quantile(0.99));
    TEST_ASSERT_EQUAL_UINT32(1, s.quantile(0));
    TEST_ASSERT_EQUAL_UINT32(1000, s.quantile(1));
}

static void test_single_value(void) {
    GapStats s;
    s.add(1234);
    TEST_ASSERT_EQUAL_UINT32(1234, s.quantile(0.5));
    TEST_ASSERT_EQUAL_UINT32(1234, s.quantile(0.99));
}

// One rotation of the ring as phase 1 saw it, with a token retry and an
// unanswered Poll For Master, each silence chosen so the categories cannot
// be confused.
static void test_categories_on_a_known_bus(void) {
    support::Wire w;
    w.send(frame(0, 1, 0));                                   // Token 0->1
    const uint64_t tokenTo1a = w.t - 8 * 130;                 // its start
    w.idle(1500);
    w.send(frame(0, 2, 1));                                   // Token 1->2
    w.idle(800);
    w.send(frame(1, 3, 2));                                   // PFM 2->3, unanswered
    w.idle(20000);
    w.send(frame(0, 0, 2));                                   // Token 2->0
    w.idle(600);
    w.send(frame(6, 1, 0, delta(14, {0x00, 0x05, 0x00})));    // service 14 seq 5
    const uint64_t end14 = w.t;
    w.idle(200);
    w.send(frame(0, 1, 0));                                   // Token 0->1
    const uint64_t tokenTo1b = w.t - 8 * 130;
    w.idle(3000);
    w.send(frame(0, 1, 0));                                   // ... retried
    const uint64_t tokenTo1c = w.t - 8 * 130;
    w.idle(2500);
    const uint64_t start15 = w.t;
    w.send(frame(6, 0, 1, delta(15, {0x01, 0x00, 0x05, 0x00, 0x02, 0x00})));

    Timing t(76800);
    TEST_ASSERT_EQUAL_UINT32(130, t.charUs());
    feed(t, w.out);

    const GapStats &tok1 = t.stats(Gap::TokenTo1);
    TEST_ASSERT_EQUAL_UINT32(2, tok1.count());
    TEST_ASSERT_EQUAL_UINT32(1500, tok1.min());
    TEST_ASSERT_EQUAL_UINT32(2500, tok1.max());
    TEST_ASSERT_EQUAL_UINT32(800, t.stats(Gap::TokenTo2).max());
    TEST_ASSERT_EQUAL_UINT32(600, t.stats(Gap::TokenTo0).max());
    TEST_ASSERT_EQUAL_UINT32(20000, t.stats(Gap::PfmSilence).max());
    TEST_ASSERT_EQUAL_UINT32(200, t.stats(Gap::AfterData0).max());
    TEST_ASSERT_EQUAL_UINT32(1, t.stats(Gap::TokenRetry).count());
    TEST_ASSERT_EQUAL_UINT32(3000, t.stats(Gap::TokenRetry).max());
    TEST_ASSERT_EQUAL_UINT32(2500, t.lastGapUs());
    TEST_ASSERT_EQUAL_UINT32(0, t.stats(Gap::Other).count());
    TEST_ASSERT_EQUAL_UINT32(7, t.stats(Gap::Any).count());
    TEST_ASSERT_EQUAL_UINT32(200, t.stats(Gap::Any).min());

    // From the end of the 14 to the start of the 15 that echoes its number.
    TEST_ASSERT_EQUAL_UINT32(1, t.stats(Gap::Ack1).count());
    TEST_ASSERT_EQUAL_UINT32(start15 - end14, t.stats(Gap::Ack1).max());

    const GapStats &rot1 = t.stats(Gap::Rotation1);
    TEST_ASSERT_EQUAL_UINT32(2, rot1.count());
    TEST_ASSERT_EQUAL_UINT32(tokenTo1b - tokenTo1a, rot1.max());
    TEST_ASSERT_EQUAL_UINT32(tokenTo1c - tokenTo1b, rot1.min());

    TEST_ASSERT_EQUAL_UINT32(0, t.stats(Gap::InFrame0).max());
    TEST_ASSERT_EQUAL_UINT32(4, t.stats(Gap::InFrame0).count());
}

static void test_silence_inside_a_frame(void) {
    support::Wire w;
    const std::vector<uint8_t> f = frame(6, 0, 2, delta(15, {0x02, 0x00, 0x05, 0x00, 0x02, 0x00}));
    w.send(std::vector<uint8_t>(f.begin(), f.begin() + 10));
    w.idle(700);
    w.send(std::vector<uint8_t>(f.begin() + 10, f.end()));
    Timing t;
    feed(t, w.out);
    TEST_ASSERT_EQUAL_UINT32(700, t.stats(Gap::InFrame2).max());
}

// Interrupt latency can stamp a byte a little late, making a back-to-back
// gap come out negative. It is recorded as zero, not as 4 billion.
static void test_negative_gap_is_zero(void) {
    support::Wire w;
    w.send(frame(0, 1, 0));
    w.send(frame(0, 2, 1), 120);  // 120 us per byte: 10 us "early"
    Timing t;
    feed(t, w.out);
    TEST_ASSERT_EQUAL_UINT32(1, t.stats(Gap::TokenTo1).count());
    TEST_ASSERT_EQUAL_UINT32(0, t.stats(Gap::TokenTo1).max());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_bins_cover_every_value);
    RUN_TEST(test_quantiles_within_a_bin);
    RUN_TEST(test_single_value);
    RUN_TEST(test_categories_on_a_known_bus);
    RUN_TEST(test_silence_inside_a_frame);
    RUN_TEST(test_negative_gap_is_zero);
    return UNITY_END();
}
