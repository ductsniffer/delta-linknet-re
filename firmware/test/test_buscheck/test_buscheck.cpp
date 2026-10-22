// The two lost-frame detectors of BusCheck, and the acknowledgement count.
#include <unity.h>

#include <vector>

#include "../Support.h"
#include "BusCheck.h"

using namespace linknet;
using support::delta;
using support::frame;

namespace {

BusCounts run(const std::vector<std::vector<uint8_t>> &frames) {
    FrameDecoder d;
    BusCheck c;
    for (const auto &f : frames) {
        for (uint8_t b : f) {
            if (!d.push(b, 0)) continue;
            const Frame &fr = d.frame();
            DeltaMessage m;
            const bool isDelta = fr.len && fr.dataCrcOk && parseDelta(fr.data, fr.len, m);
            c.onFrame(fr, isDelta ? &m : nullptr);
        }
    }
    return c.counts();
}

std::vector<uint8_t> poll14(uint8_t zone, uint16_t seq) {
    return frame(6, zone, 0, delta(14, {0x00, static_cast<uint8_t>(seq), static_cast<uint8_t>(seq >> 8)}));
}

std::vector<uint8_t> ack15(uint8_t zone, uint16_t seq) {
    return frame(6, 0, zone,
                 delta(15, {zone, 0x00, static_cast<uint8_t>(seq), static_cast<uint8_t>(seq >> 8), 0x02, 0x00}));
}

const std::vector<uint8_t> T01 = frame(0, 1, 0), T12 = frame(0, 2, 1), T20 = frame(0, 0, 2);

}  // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_a_healthy_ring(void) {
    const BusCounts c = run({T01, T12, frame(1, 3, 2), T20, T01, T12, T20});
    TEST_ASSERT_EQUAL_UINT32(7, c.frames);
    TEST_ASSERT_EQUAL_UINT32(6, c.byType[0]);
    TEST_ASSERT_EQUAL_UINT32(1, c.byType[1]);
    TEST_ASSERT_EQUAL_UINT32(0, c.ringBreaks);
    TEST_ASSERT_EQUAL_UINT32(0, c.tokenRetries);
}

// A Token 1->2 the receiver missed: 2 then seems to speak out of turn.
static void test_a_missed_token_breaks_the_ring(void) {
    const BusCounts c = run({T01, /* T12 lost */ T20, T01, T12});
    TEST_ASSERT_EQUAL_UINT32(1, c.ringBreaks);
}

// What the capture holds four times: Token 0->1, Token 0->1.
static void test_token_retry_is_not_a_break(void) {
    const BusCounts c = run({T20, T01, T01, T12, T20});
    TEST_ASSERT_EQUAL_UINT32(0, c.ringBreaks);
    TEST_ASSERT_EQUAL_UINT32(1, c.tokenRetries);
}

// After an unanswered token the passer may also look for another
// successor with a Poll For Master (ASHRAE 135 9.5.6).
static void test_poll_after_unanswered_token_is_not_a_break(void) {
    const BusCounts c = run({T20, T01, frame(1, 2, 0), T01, T12});
    TEST_ASSERT_EQUAL_UINT32(0, c.ringBreaks);
    TEST_ASSERT_EQUAL_UINT32(1, c.tokenRetries);
}

static void test_reply_to_poll_for_master_is_not_a_break(void) {
    const BusCounts c = run({T12, frame(1, 3, 2), frame(2, 2, 3), T20});
    TEST_ASSERT_EQUAL_UINT32(0, c.ringBreaks);
}

static void test_sequence_jump(void) {
    const BusCounts c = run({poll14(1, 0xfffe), ack15(1, 0xfffe), poll14(2, 0xffff), ack15(2, 0xffff),
                             poll14(1, 0x0000), ack15(1, 0x0000),  // wraps cleanly
                             poll14(2, 0x0003), ack15(2, 0x0003)});  // 1 and 2 missed
    TEST_ASSERT_EQUAL_UINT32(1, c.seqJumps);
    TEST_ASSERT_EQUAL_UINT32(2, c.seqMissing);
    TEST_ASSERT_EQUAL_UINT32(4, c.acked);
    TEST_ASSERT_EQUAL_UINT32(8, c.delta);
}

static void test_acknowledgements(void) {
    const BusCounts c = run({poll14(1, 10), ack15(1, 10),  // acked
                             poll14(2, 11), poll14(2, 12),  // the 11 never acked
                             ack15(2, 12), ack15(1, 99)});  // 99 answers nothing
    TEST_ASSERT_EQUAL_UINT32(2, c.acked);
    TEST_ASSERT_EQUAL_UINT32(1, c.unacked);
    TEST_ASSERT_EQUAL_UINT32(1, c.ackMismatch);
    TEST_ASSERT_EQUAL_UINT32(0, c.seqJumps);
}

static void test_non_delta_data_is_counted_apart(void) {
    const BusCounts c = run({frame(6, 0, 1, {0x01, 0x00, 0x10, 0x08})});  // a Who-Is
    TEST_ASSERT_EQUAL_UINT32(1, c.otherData);
    TEST_ASSERT_EQUAL_UINT32(0, c.delta);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_a_healthy_ring);
    RUN_TEST(test_a_missed_token_breaks_the_ring);
    RUN_TEST(test_token_retry_is_not_a_break);
    RUN_TEST(test_poll_after_unanswered_token_is_not_a_break);
    RUN_TEST(test_reply_to_poll_for_master_is_not_a_break);
    RUN_TEST(test_sequence_jump);
    RUN_TEST(test_acknowledgements);
    RUN_TEST(test_non_delta_data_is_counted_apart);
    return UNITY_END();
}
