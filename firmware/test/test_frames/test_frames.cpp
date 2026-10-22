// The example frames of docs/example-frames.md, read from the file itself so the
// test and the document cannot drift apart: CRCs, header fields, and the
// Delta payload of each service.
#include <unity.h>

#include <string>
#include <vector>

#include "../Support.h"
#include "Crc.h"
#include "DeltaPrivate.h"
#include "Mstp.h"

using namespace linknet;

namespace {

struct Example {
    std::string title;
    std::vector<uint8_t> bytes;
};

std::vector<Example> g_examples;

void loadExamples() {
    const std::string md = support::readText(support::path("docs/example-frames.md"));
    std::string title;
    bool inBlock = false;
    for (const std::string &l : support::lines(md)) {
        if (l.rfind("## ", 0) == 0) {
            title = l.substr(3);
        } else if (l.rfind("```", 0) == 0) {
            inBlock = !inBlock;
        } else if (inBlock && !l.empty()) {
            g_examples.push_back(Example{title, support::hex(l)});
        }
    }
}

const Example &find(const char *needle) {
    for (const Example &e : g_examples) {
        if (e.title.find(needle) != std::string::npos) return e;
    }
    TEST_FAIL_MESSAGE(needle);
    return g_examples.front();
}

// Decodes one example on its own and returns the single frame it holds.
Frame decodeOne(const Example &e) {
    FrameDecoder d;
    int frames = 0;
    Frame f;
    for (size_t i = 0; i < e.bytes.size(); ++i) {
        if (d.push(e.bytes[i], i)) {
            ++frames;
            f = d.frame();
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(e.bytes.size() - 1, i, "frame ended early");
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, frames, e.title.c_str());
    return f;
}

DeltaMessage deltaOf(const Frame &f) {
    DeltaMessage m;
    TEST_ASSERT_TRUE(parseDelta(f.data, f.len, m));
    return m;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_file_has_seven_examples(void) {
    TEST_ASSERT_EQUAL_UINT32(7, g_examples.size());
}

static void test_crcs_of_every_example(void) {
    for (const Example &e : g_examples) {
        const std::vector<uint8_t> &b = e.bytes;
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x55, b[0], e.title.c_str());
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, b[1], e.title.c_str());
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(b[7], headerCrc(&b[2], 5), e.title.c_str());
        const size_t len = static_cast<size_t>(b[5] << 8 | b[6]);
        if (len == 0) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(8, b.size(), e.title.c_str());
            continue;
        }
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(8 + len + 2, b.size(), e.title.c_str());
        const uint16_t sent = static_cast<uint16_t>(b[8 + len] | b[9 + len] << 8);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(sent, dataCrc(&b[8], len), e.title.c_str());
    }
}

static void test_token(void) {
    const Frame f = decodeOne(find("Token"));
    TEST_ASSERT_EQUAL_UINT8(0, f.type);
    TEST_ASSERT_EQUAL_UINT8(1, f.src);
    TEST_ASSERT_EQUAL_UINT8(2, f.dst);
    TEST_ASSERT_EQUAL_UINT16(0, f.len);
    TEST_ASSERT_TRUE(f.dataCrcOk);
    TEST_ASSERT_EQUAL_STRING("Token", frameTypeName(f.type));
}

static void test_poll_for_master(void) {
    const Frame f = decodeOne(find("PollForMaster"));
    TEST_ASSERT_EQUAL_UINT8(1, f.type);
    TEST_ASSERT_EQUAL_UINT8(2, f.src);
    TEST_ASSERT_EQUAL_UINT8(3, f.dst);
    TEST_ASSERT_EQUAL_STRING("PollForMaster", frameTypeName(f.type));
}

static void test_service_14(void) {
    const Frame f = decodeOne(find("service 14"));
    TEST_ASSERT_EQUAL_UINT8(6, f.type);
    TEST_ASSERT_EQUAL_UINT8(0, f.src);
    TEST_ASSERT_EQUAL_UINT8(1, f.dst);
    TEST_ASSERT_TRUE(f.dataCrcOk);
    const DeltaMessage m = deltaOf(f);
    TEST_ASSERT_EQUAL_UINT8(14, m.service);
    // Tag 63: a 3-byte string, length in the tag byte (ASHRAE 135 20.2.1).
    TEST_ASSERT_EQUAL_UINT16(3, m.len);
    TEST_ASSERT_EQUAL_STRING("00aafc", support::toHex(m.payload, m.len).c_str());
    PollRequest p;
    TEST_ASSERT_TRUE(decodePoll(m, p));
    TEST_ASSERT_EQUAL_HEX16(0xfcaa, p.seq);
}

static void test_service_15(void) {
    const Frame f = decodeOne(find("service 15"));
    TEST_ASSERT_EQUAL_UINT8(1, f.src);
    TEST_ASSERT_EQUAL_UINT8(0, f.dst);
    const DeltaMessage m = deltaOf(f);
    TEST_ASSERT_EQUAL_UINT8(15, m.service);
    AckReport a;
    TEST_ASSERT_TRUE(decodeAck(m, a));
    TEST_ASSERT_EQUAL_UINT8(1, a.zone);
    TEST_ASSERT_EQUAL_HEX16(0xfcaa, a.seq);  // echoes the service 14 above
    TEST_ASSERT_EQUAL_UINT8(2, a.status);
}

static void test_service_13(void) {
    const Frame f = decodeOne(find("service 13"));
    TEST_ASSERT_EQUAL_UINT8(0, f.src);
    TEST_ASSERT_EQUAL_UINT8(2, f.dst);
    const DeltaMessage m = deltaOf(f);
    TEST_ASSERT_EQUAL_UINT8(13, m.service);
    TEST_ASSERT_EQUAL_UINT16(56, m.len);
    DisplayUpdate d;
    TEST_ASSERT_TRUE(decodeDisplay(m, d));
    TEST_ASSERT_EQUAL_STRING("5.4", d.outdoorText);
    TEST_ASSERT_EQUAL_STRING("21.6", d.indoorText);
    TEST_ASSERT_EQUAL_STRING("20.0", d.setpointText);
    TEST_ASSERT_TRUE(d.outdoor.valid);
    TEST_ASSERT_EQUAL_INT16(54, d.outdoor.tenths);
    TEST_ASSERT_EQUAL_INT16(216, d.indoor.tenths);
    TEST_ASSERT_EQUAL_INT16(200, d.setpoint.tenths);
    TEST_ASSERT_TRUE(d.fan);
    TEST_ASSERT_TRUE(d.on);
    TEST_ASSERT_FALSE(d.adjusting);
    TEST_ASSERT_EQUAL_HEX16(0xfcac, d.seq);
    TEST_ASSERT_EQUAL_HEX64(0, d.unexpected);
}

static void test_service_12_reading(void) {
    const Frame f = decodeOne(find("service 12 — 1"));
    const DeltaMessage m = deltaOf(f);
    SensorReport s;
    TEST_ASSERT_TRUE(decodeSensor(m, s));
    TEST_ASSERT_EQUAL_UINT8(1, s.zone);
    TEST_ASSERT_FALSE(s.keyPress);
    TEST_ASSERT_EQUAL_UINT16(0x088b, s.raw);
    TEST_ASSERT_EQUAL_UINT8(0, s.keyMask);
    TEST_ASSERT_EQUAL_HEX64(0, s.unexpected);
}

static void test_service_12_key(void) {
    const Frame f = decodeOne(find("key press"));
    const DeltaMessage m = deltaOf(f);
    SensorReport s;
    TEST_ASSERT_TRUE(decodeSensor(m, s));
    TEST_ASSERT_EQUAL_UINT8(1, s.zone);
    TEST_ASSERT_TRUE(s.keyPress);
    TEST_ASSERT_EQUAL_UINT16(0x089d, s.raw);
    TEST_ASSERT_EQUAL_HEX8(0x08, s.keyMask);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Key::Up), static_cast<int>(s.key));
    TEST_ASSERT_EQUAL_HEX64(0, s.unexpected);
}

// All seven back to back, as on the wire: seven frames, at the right offsets.
static void test_examples_as_one_stream(void) {
    FrameDecoder d;
    std::vector<uint32_t> offsets;
    uint32_t expected = 0;
    std::vector<uint32_t> starts;
    for (const Example &e : g_examples) {
        starts.push_back(expected);
        for (uint8_t b : e.bytes) {
            if (d.push(b, 0)) offsets.push_back(d.frame().offset);
        }
        expected += static_cast<uint32_t>(e.bytes.size());
    }
    TEST_ASSERT_EQUAL_UINT32(starts.size(), offsets.size());
    for (size_t i = 0; i < starts.size(); ++i) TEST_ASSERT_EQUAL_UINT32(starts[i], offsets[i]);
    TEST_ASSERT_EQUAL_UINT32(0, d.stats().bytesOutsideFrames());
    TEST_ASSERT_EQUAL_UINT32(0, d.stats().headerCrcErrors);
    TEST_ASSERT_EQUAL_UINT32(0, d.stats().dataCrcErrors);
}

static void test_flipped_data_bit_fails_the_data_crc(void) {
    std::vector<uint8_t> b = find("service 13").bytes;
    b[30] ^= 0x01;  // a digit of the indoor temperature
    FrameDecoder d;
    int frames = 0;
    for (uint8_t x : b) frames += d.push(x, 0);
    TEST_ASSERT_EQUAL_INT(1, frames);
    TEST_ASSERT_FALSE(d.frame().dataCrcOk);
    TEST_ASSERT_EQUAL_UINT32(1, d.stats().dataCrcErrors);
}

static void test_flipped_header_bit_drops_the_frame(void) {
    std::vector<uint8_t> b = find("Token").bytes;
    b[3] ^= 0x01;  // destination 2 -> 3
    const std::vector<uint8_t> next = find("PollForMaster").bytes;
    b.insert(b.end(), next.begin(), next.end());
    FrameDecoder d;
    std::vector<Frame> got;
    for (uint8_t x : b) {
        if (d.push(x, 0)) got.push_back(d.frame());
    }
    TEST_ASSERT_EQUAL_UINT32(1, got.size());
    TEST_ASSERT_EQUAL_UINT8(1, got[0].type);  // the Poll For Master survived
    TEST_ASSERT_EQUAL_UINT32(8, got[0].offset);
    TEST_ASSERT_EQUAL_UINT32(1, d.stats().headerCrcErrors);
    TEST_ASSERT_EQUAL_UINT32(8, d.stats().bytesOutsideFrames());
}

int main(int, char **) {
    loadExamples();
    UNITY_BEGIN();
    RUN_TEST(test_file_has_seven_examples);
    RUN_TEST(test_crcs_of_every_example);
    RUN_TEST(test_token);
    RUN_TEST(test_poll_for_master);
    RUN_TEST(test_service_14);
    RUN_TEST(test_service_15);
    RUN_TEST(test_service_13);
    RUN_TEST(test_service_12_reading);
    RUN_TEST(test_service_12_key);
    RUN_TEST(test_examples_as_one_stream);
    RUN_TEST(test_flipped_data_bit_fails_the_data_crc);
    RUN_TEST(test_flipped_header_bit_drops_the_frame);
    return UNITY_END();
}
