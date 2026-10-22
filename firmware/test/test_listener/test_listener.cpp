// The console as the firmware prints it: one line per zone change, key
// presses, oddities, bus silence, the statistics block, and the LED colour.
#include <unity.h>

#include <cstring>
#include <vector>

#include "../Support.h"
#include "Listener.h"

using namespace linknet;
using support::delta;
using support::frame;

namespace {

std::vector<uint8_t> display(const char *indoor, const char *setpoint, bool fan, bool on,
                             bool adjusting, uint16_t seq, uint8_t byte0 = 0) {
    std::vector<uint8_t> p(kDisplayLen, 0);
    p[0] = byte0;
    std::memcpy(&p[6], "5.4", 3);
    std::memcpy(&p[14], indoor, std::strlen(indoor));
    std::memcpy(&p[22], setpoint, std::strlen(setpoint));
    p[38] = 0x01;
    p[39] = fan;
    p[43] = on;
    p[45] = adjusting ? 0xff : 0x00;
    p[49] = static_cast<uint8_t>(seq);
    p[50] = static_cast<uint8_t>(seq >> 8);
    p[53] = 0x01;
    p[54] = 0x01;
    p[55] = 0x02;
    return delta(13, p);
}

std::vector<uint8_t> sensor(uint8_t zone, uint16_t raw, uint8_t keyMask = 0, uint8_t keyNo = 0) {
    std::vector<uint8_t> p(kSensorLen, 0);
    p[1] = zone;
    p[2] = 0x02;
    p[3] = keyNo ? 0x00 : 0x01;
    p[5] = static_cast<uint8_t>(raw);
    p[6] = static_cast<uint8_t>(raw >> 8);
    p[39] = keyMask;
    p[41] = keyNo;
    return delta(12, p);
}

struct Bench {
    support::CaptureLog log;
    Listener listener{log};
    support::Wire wire;

    void send(const std::vector<uint8_t> &f, uint32_t idleUs = 1000) {
        wire.out.clear();
        wire.send(f);
        wire.idle(idleUs);
        support::VectorSource src(wire.out, 7);
        listener.poll(src);
    }
    std::string last() const { return log.lines.empty() ? "" : log.lines.back(); }
};

}  // namespace

void setUp(void) {}
void tearDown(void) {}

static void test_first_display_prints_everything(void) {
    Bench b;
    b.send(frame(6, 2, 0, display("21.6", "20.0", true, true, false, 1)));
    TEST_ASSERT_EQUAL_UINT32(1, b.log.lines.size());
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          b.last().find("Z2 living in=21.6 sp=20.0 out=5.4 fan=1 on=1 adj=0 raw=? "
                                        "(first in sp out fan on adj)"));
}

static void test_only_changes_print(void) {
    Bench b;
    b.send(frame(6, 2, 0, display("21.6", "20.0", true, true, false, 1)));
    b.send(frame(6, 2, 0, display("21.6", "20.0", true, true, false, 2)));  // same again
    TEST_ASSERT_EQUAL_UINT32(1, b.log.lines.size());
    b.send(frame(6, 2, 0, display("21.6", "20.1", true, true, true, 3)));
    TEST_ASSERT_EQUAL_UINT32(2, b.log.lines.size());
    TEST_ASSERT_NOT_EQUAL(std::string::npos, b.last().find("sp=20.1"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, b.last().find("(sp adj)"));
    const ZoneState *z = b.listener.zones().zone(2);
    TEST_ASSERT_EQUAL_INT16(201, z->setpoint.tenths);
    TEST_ASSERT_EQUAL_UINT32(3, z->displays);
}

static void test_zones_are_kept_apart(void) {
    Bench b;
    b.send(frame(6, 1, 0, display("21.3", "20.3", false, false, false, 1)));
    b.send(frame(6, 2, 0, display("20.9", "19.4", true, true, false, 2)));
    TEST_ASSERT_EQUAL_STRING("20.3", b.listener.zones().zone(1)->setpointText);
    TEST_ASSERT_EQUAL_STRING("19.4", b.listener.zones().zone(2)->setpointText);
    TEST_ASSERT_FALSE(b.listener.zones().zone(1)->fan);
    TEST_ASSERT_TRUE(b.listener.zones().zone(2)->fan);
}

static void test_key_press_and_raw_reading(void) {
    Bench b;
    b.send(frame(6, 0, 1, sensor(1, 2187)));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, b.last().find("Z1 bedroom in=? sp=? out=? fan=? on=? adj=? raw=2187 (raw)"));
    b.send(frame(6, 0, 1, sensor(1, 2205, 0x08, 4)));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, b.log.lines[b.log.lines.size() - 2].find("raw=2205 (raw)"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, b.last().find("Z1 bedroom key=UP mask=08 raw=2205"));
    b.send(frame(6, 0, 1, sensor(1, 2205, 0x02, 3)));  // same raw: only the key line
    TEST_ASSERT_NOT_EQUAL(std::string::npos, b.last().find("key=DOWN mask=02"));
    TEST_ASSERT_EQUAL_UINT32(4, b.log.lines.size());
    TEST_ASSERT_EQUAL_UINT32(2, b.listener.zones().zone(1)->keys);
}

static void test_odd_bytes_reported_once(void) {
    Bench b;
    b.send(frame(6, 2, 0, display("21.6", "20.0", true, true, false, 1, 0x07)));
    TEST_ASSERT_TRUE(b.log.any("Z2 ODD service 13 unusual bytes 0: 07000000"));
    const size_t n = b.log.lines.size();
    b.send(frame(6, 2, 0, display("21.6", "20.0", true, true, false, 2, 0x07)));
    TEST_ASSERT_EQUAL_UINT32(n, b.log.lines.size());
    b.send(frame(6, 2, 0, display("21.6", "20.0", true, true, false, 3)));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, b.last().find("Z2 ODD service 13 back to the usual bytes"));
}

static void test_sensor_naming_another_zone(void) {
    Bench b;
    b.send(frame(6, 0, 1, sensor(2, 2200)));  // from address 1, byte 1 says 2
    TEST_ASSERT_TRUE(b.log.any("Z1 ODD service 12 from address 1 names zone 2 in byte 1"));
}

static void test_retry_and_data_crc_lines(void) {
    Bench b;
    b.send(frame(0, 1, 0), 3200);
    b.send(frame(0, 1, 0));
    TEST_ASSERT_TRUE(b.log.any("RETRY Token 0->1, 3200 us after the unanswered token"));
    std::vector<uint8_t> bad = frame(6, 2, 0, display("21.6", "20.0", true, true, false, 1));
    bad[20] ^= 0x40;
    b.send(bad);
    TEST_ASSERT_TRUE(b.log.any("ERR data CRC 0->2 type 6 len 68"));
    TEST_ASSERT_FALSE(b.listener.zones().zone(2)->haveDisplay);
}

static void test_silence_and_return(void) {
    Bench b;
    b.send(frame(0, 1, 0));
    const uint64_t last = b.listener.decoder().frame().lastByteUs;
    b.listener.tick(last + 900000, RxHealth());
    TEST_ASSERT_FALSE(b.log.any("BUS silent"));
    b.listener.tick(last + 1100000, RxHealth());
    TEST_ASSERT_TRUE(b.log.any("BUS silent: no frame for 1000 ms"));
    b.listener.tick(last + 1200000, RxHealth());
    TEST_ASSERT_EQUAL_UINT32(1, b.log.count("BUS silent"));
    b.wire.idle(5000000);
    b.send(frame(0, 2, 1));
    TEST_ASSERT_TRUE(b.log.any("BUS back after 5001 ms of silence"));
}

static void test_statistics_block(void) {
    Bench b;
    b.send(frame(0, 0, 2));  // Token 2->0, then the controller speaks
    b.send(frame(6, 2, 0, display("21.6", "20.0", true, true, false, 1)));
    const uint64_t t0 = b.wire.t;
    b.log.lines.clear();
    b.listener.tick(t0, RxHealth());  // arms the period
    TEST_ASSERT_EQUAL_UINT32(0, b.log.lines.size());
    RxHealth rx;
    rx.interrupts = 12;
    rx.framingErrors = 1;
    b.listener.tick(t0 + 60000000, rx);
    TEST_ASSERT_TRUE(b.log.any("STATS frames=2 token=1 pfm=0 data=1 hdr_crc=0"));
    TEST_ASSERT_TRUE(b.log.any("UART bytes=86 irq=12 multi=0 timeout=0 fifo_ovf=0 ring_ovf=0 framing=1"));
    TEST_ASSERT_TRUE(b.log.any("Z1 bedroom in=? sp=? out=? fan=? on=? adj=? raw=? last13=never n12=0 keys=0 STALE"));
    TEST_ASSERT_TRUE(b.log.any("Z2 living in=21.6 sp=20.0 out=5.4 fan=1 on=1 adj=0 raw=? last13=60.0s n13=1"));
    TEST_ASSERT_FALSE(b.log.any("last13=60.0s n13=1 n12=0 keys=0 STALE"));
    TEST_ASSERT_TRUE(b.log.any("GAP token>0     n=1 min=1000 p50=1000 p99=1000 max=1000 avg=1000 us"));
    const size_t n = b.log.lines.size();
    b.listener.tick(t0 + 61000000, rx);  // not due again yet
    TEST_ASSERT_EQUAL_UINT32(n, b.log.lines.size());
}

static void test_led_colour(void) {
    Bench b;
    TEST_ASSERT_TRUE(b.listener.status(0) == (Rgb{0, 0, 255}));  // nothing heard yet
    b.send(frame(0, 1, 0));
    const uint64_t t = b.listener.decoder().frame().lastByteUs;
    TEST_ASSERT_TRUE(b.listener.status(t + 1000) == (Rgb{0, 255, 0}));
    TEST_ASSERT_TRUE(b.listener.status(t + 1500000) == (Rgb{255, 0, 0}));  // silent
    RxHealth rx;
    rx.fifoOverflows = 1;
    b.listener.tick(t + 2000, rx);
    TEST_ASSERT_TRUE(b.listener.status(t + 3000) == (Rgb{255, 110, 0}));    // error
    TEST_ASSERT_TRUE(b.listener.status(t + 900000) == (Rgb{255, 110, 0}));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_first_display_prints_everything);
    RUN_TEST(test_only_changes_print);
    RUN_TEST(test_zones_are_kept_apart);
    RUN_TEST(test_key_press_and_raw_reading);
    RUN_TEST(test_odd_bytes_reported_once);
    RUN_TEST(test_sensor_naming_another_zone);
    RUN_TEST(test_retry_and_data_crc_lines);
    RUN_TEST(test_silence_and_return);
    RUN_TEST(test_statistics_block);
    RUN_TEST(test_led_colour);
    return UNITY_END();
}
