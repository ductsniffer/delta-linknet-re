// Delta message parsing beyond what the capture showed: negative outdoor
// temperatures, malformed envelopes, and bytes that phase 1 never saw move.
#include <unity.h>

#include <cstring>
#include <vector>

#include "../Support.h"
#include "DeltaPrivate.h"

using namespace linknet;

namespace {

// The service 13 of docs/example-frames.md, 0 -> 2, as a payload.
std::vector<uint8_t> displayPayload() {
    return support::hex(
        "000000000000352e34000000000032312e360000000032302e30000000000000000000000000"
        "0101000000010000000000acfc0000010102");
}

void setText(std::vector<uint8_t> &p, size_t at, const char *text) {
    for (size_t i = 0; i < kTextField; ++i) p[at + i] = 0;
    std::memcpy(&p[at], text, std::strlen(text));
}

bool decodeDisplayFrom(const std::vector<uint8_t> &payload, DisplayUpdate &d) {
    const std::vector<uint8_t> data = support::delta(13, payload);
    DeltaMessage m;
    if (!parseDelta(data.data(), data.size(), m)) return false;
    return decodeDisplay(m, d);
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_parse_deci(void) {
    struct {
        const char *text;
        bool valid;
        int tenths;
    } cases[] = {
        {"21.6", true, 216}, {"20.0", true, 200}, {"5.3", true, 53},  {"5", true, 50},
        {"0.0", true, 0},    {"-0.5", true, -5},  {"-12.5", true, -125}, {"-30", true, -300},
        {"", false, 0},      {"-", false, 0},     {"21.", false, 0},   {"21.65", false, 0},
        {".5", false, 0},    {" 5", false, 0},    {"5 ", false, 0},    {"+5", false, 0},
        {"1234", false, 0},  {"--5", false, 0},   {"ab", false, 0},
    };
    for (const auto &c : cases) {
        const Deci d = parseDeci(c.text);
        TEST_ASSERT_EQUAL_MESSAGE(c.valid, d.valid, c.text);
        if (c.valid) TEST_ASSERT_EQUAL_INT16_MESSAGE(c.tenths, d.tenths, c.text);
    }
}

// Not seen yet (docs/report.md section 9) but certain to come in January.
static void test_negative_outdoor_temperature(void) {
    std::vector<uint8_t> p = displayPayload();
    setText(p, 6, "-12.5");
    DisplayUpdate d;
    TEST_ASSERT_TRUE(decodeDisplayFrom(p, d));
    TEST_ASSERT_EQUAL_STRING("-12.5", d.outdoorText);
    TEST_ASSERT_TRUE(d.outdoor.valid);
    TEST_ASSERT_EQUAL_INT16(-125, d.outdoor.tenths);
    TEST_ASSERT_EQUAL_STRING("21.6", d.indoorText);  // the next field untouched
    TEST_ASSERT_EQUAL_HEX64(0, d.unexpected);
}

// Eight characters fill the field with no terminator at all.
static void test_text_field_of_eight_characters(void) {
    std::vector<uint8_t> p = displayPayload();
    std::memcpy(&p[6], "-123.456", 8);
    DisplayUpdate d;
    TEST_ASSERT_TRUE(decodeDisplayFrom(p, d));
    TEST_ASSERT_EQUAL_STRING("-123.456", d.outdoorText);
    TEST_ASSERT_FALSE(d.outdoor.valid);
    TEST_ASSERT_EQUAL_STRING("21.6", d.indoorText);
}

static void test_unusual_bytes_are_flagged(void) {
    std::vector<uint8_t> p = displayPayload();
    p[0] = 0x02;   // never seen non-zero
    p[38] = 0x00;  // always 01 in phase 1
    p[39] = 0x02;  // fan is a flag
    DisplayUpdate d;
    TEST_ASSERT_TRUE(decodeDisplayFrom(p, d));
    TEST_ASSERT_EQUAL_HEX64((uint64_t{1} << 0) | (uint64_t{1} << 38) | (uint64_t{1} << 39), d.unexpected);
}

static void test_adjusting_flag(void) {
    std::vector<uint8_t> p = displayPayload();
    p[45] = 0xff;
    DisplayUpdate d;
    TEST_ASSERT_TRUE(decodeDisplayFrom(p, d));
    TEST_ASSERT_TRUE(d.adjusting);
    TEST_ASSERT_EQUAL_HEX64(0, d.unexpected);
}

static void test_sensor_unusual_bytes(void) {
    std::vector<uint8_t> p(kSensorLen, 0);
    p[1] = 0x02;
    p[2] = 0x02;
    p[3] = 0x01;
    p[5] = 0x9d;
    p[6] = 0x08;
    p[20] = 0x01;  // never seen
    const std::vector<uint8_t> data = support::delta(12, p);
    DeltaMessage m;
    TEST_ASSERT_TRUE(parseDelta(data.data(), data.size(), m));
    SensorReport s;
    TEST_ASSERT_TRUE(decodeSensor(m, s));
    TEST_ASSERT_EQUAL_UINT8(2, s.zone);
    TEST_ASSERT_FALSE(s.keyPress);
    TEST_ASSERT_EQUAL_UINT16(2205, s.raw);
    TEST_ASSERT_EQUAL_HEX64(uint64_t{1} << 20, s.unexpected);
}

static void test_envelope_rejections(void) {
    const std::vector<uint8_t> good = support::delta(14, {0x00, 0xaa, 0xfc});
    DeltaMessage m;
    TEST_ASSERT_TRUE(parseDelta(good.data(), good.size(), m));

    std::vector<uint8_t> b = good;
    b[5] = 0x07;  // another vendor
    TEST_ASSERT_FALSE(parseDelta(b.data(), b.size(), m));
    b = good;
    b.back() = 0x3f;  // wrong closing tag
    TEST_ASSERT_FALSE(parseDelta(b.data(), b.size(), m));
    b = good;
    b.pop_back();  // truncated
    TEST_ASSERT_FALSE(parseDelta(b.data(), b.size(), m));
    b = good;
    b.insert(b.end() - 1, 0x00);  // one byte more than announced
    TEST_ASSERT_FALSE(parseDelta(b.data(), b.size(), m));
    b = good;
    b[9] = 0x64;  // announces 4, carries 3
    TEST_ASSERT_FALSE(parseDelta(b.data(), b.size(), m));
    b = good;
    b[9] = 0x43;  // application tag 4 (real), not an octet string
    TEST_ASSERT_FALSE(parseDelta(b.data(), b.size(), m));
}

static void test_wrong_length_is_not_decoded(void) {
    const std::vector<uint8_t> data = support::delta(13, std::vector<uint8_t>(55, 0));
    DeltaMessage m;
    TEST_ASSERT_TRUE(parseDelta(data.data(), data.size(), m));
    DisplayUpdate d;
    TEST_ASSERT_FALSE(decodeDisplay(m, d));
    SensorReport s;
    TEST_ASSERT_FALSE(decodeSensor(m, s));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_deci);
    RUN_TEST(test_negative_outdoor_temperature);
    RUN_TEST(test_text_field_of_eight_characters);
    RUN_TEST(test_unusual_bytes_are_flagged);
    RUN_TEST(test_adjusting_flag);
    RUN_TEST(test_sensor_unusual_bytes);
    RUN_TEST(test_envelope_rejections);
    RUN_TEST(test_wrong_length_is_not_decoded);
    return UNITY_END();
}
