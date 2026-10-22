// The network console's queue: lines kept whole, gaps announced, storage
// reused across the wrap.
#include <unity.h>

#include <string>

#include "LineQueue.h"

using namespace linknet;

namespace {

std::string drain(LineQueue &q, size_t chunk = 1000) {
    std::string out;
    char buf[1000];
    while (q.size() > 0) {
        const size_t n = q.peek(buf, chunk < sizeof buf ? chunk : sizeof buf);
        out.append(buf, n);
        q.consume(n);
    }
    return out;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_lines_come_out_in_order_with_newlines(void) {
    char storage[64];
    LineQueue q(storage, sizeof storage);
    TEST_ASSERT_TRUE(q.push("one"));
    TEST_ASSERT_TRUE(q.push(""));
    TEST_ASSERT_TRUE(q.push("three"));
    TEST_ASSERT_EQUAL_UINT32(4 + 1 + 6, q.size());
    TEST_ASSERT_EQUAL_STRING("one\n\nthree\n", drain(q).c_str());
}

static void test_peek_does_not_remove(void) {
    char storage[32];
    LineQueue q(storage, sizeof storage);
    q.push("abcdef");
    char buf[4];
    TEST_ASSERT_EQUAL_UINT32(4, q.peek(buf, sizeof buf));
    TEST_ASSERT_EQUAL_UINT32(4, q.peek(buf, sizeof buf));
    TEST_ASSERT_EQUAL_UINT32(7, q.size());
    // The network took only part of what it was offered.
    q.consume(2);
    TEST_ASSERT_EQUAL_STRING("cdef\n", drain(q).c_str());
}

static void test_a_line_that_does_not_fit_is_dropped_whole(void) {
    char storage[16];
    LineQueue q(storage, sizeof storage);
    TEST_ASSERT_TRUE(q.push("0123456789"));  // 11 bytes
    TEST_ASSERT_FALSE(q.push("abcdef"));     // 7 more would make 18
    TEST_ASSERT_EQUAL_UINT32(1, q.droppedLines());
    TEST_ASSERT_EQUAL_STRING("0123456789\n", drain(q).c_str());
}

static void test_the_gap_is_announced_before_the_next_line(void) {
    char storage[128];
    LineQueue q(storage, sizeof storage);
    TEST_ASSERT_TRUE(q.push(std::string(125, 'x').c_str()));  // 126 of 128 bytes
    TEST_ASSERT_FALSE(q.push("lost 1"));
    TEST_ASSERT_FALSE(q.push("lost 2"));
    drain(q);
    TEST_ASSERT_TRUE(q.push("back"));
    TEST_ASSERT_EQUAL_STRING("NET 2 console lines dropped here (queue full)\nback\n", drain(q).c_str());
    TEST_ASSERT_EQUAL_UINT32(2, q.droppedLines());
    // Announced once only.
    q.push("next");
    TEST_ASSERT_EQUAL_STRING("next\n", drain(q).c_str());
}

static void test_no_room_for_notice_and_line_keeps_counting(void) {
    char storage[48];
    LineQueue q(storage, sizeof storage);
    q.push(std::string(46, 'x').c_str());  // 47 of 48 bytes
    TEST_ASSERT_FALSE(q.push("a"));
    drain(q);
    // The notice (46 bytes) and this line no longer fit together: both wait.
    TEST_ASSERT_FALSE(q.push("0123"));
    TEST_ASSERT_EQUAL_UINT32(2, q.droppedLines());
    TEST_ASSERT_TRUE(q.push(""));
    TEST_ASSERT_EQUAL_STRING("NET 2 console lines dropped here (queue full)\n\n", drain(q).c_str());
}

static void test_wraps_around_the_storage(void) {
    char storage[10];
    LineQueue q(storage, sizeof storage);
    std::string all;
    for (int i = 0; i < 50; ++i) {
        const std::string line = std::to_string(i * 37 % 1000);
        TEST_ASSERT_TRUE(q.push(line.c_str()));
        // Drain in small pieces so head and tail cross the end at odd places.
        all += drain(q, 3);
        TEST_ASSERT_EQUAL_STRING((line + "\n").c_str(), all.substr(all.size() - line.size() - 1).c_str());
    }
    TEST_ASSERT_EQUAL_UINT32(0, q.droppedLines());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_lines_come_out_in_order_with_newlines);
    RUN_TEST(test_peek_does_not_remove);
    RUN_TEST(test_a_line_that_does_not_fit_is_dropped_whole);
    RUN_TEST(test_the_gap_is_announced_before_the_next_line);
    RUN_TEST(test_no_room_for_notice_and_line_keeps_counting);
    RUN_TEST(test_wraps_around_the_storage);
    return UNITY_END();
}
