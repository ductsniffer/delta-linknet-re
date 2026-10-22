// Replays the phase 1 capture, captures/phase1-usb/20261016-211923.bin,
// through the decoder and checks it against the reference files made from
// it by the phase 1 Python scripts, in the same folder:
//
//   expected_summary.txt         byte, frame and CRC counts, per type,
//                                source and destination
//   expected_delta_messages.csv  every Delta message, line by line
//   markers.csv                  the tests, as offsets in the .bin
//
// The .bin has no timestamps. The "time" column of the CSV is the time of
// the read that brought the frame's last byte, which only the .log knows,
// so the .log is parsed for it the way mstp.read_log() does. Timing
// itself cannot be checked here (the capture's 16 ms resolution is the very
// reason the ESP32 measures it); test_timing covers it.
//
// Set LINKNET_REPLAY_OUT to a path to also get the console output the
// firmware would have printed over the capture.
#include <unity.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "../Support.h"
#include "BusCheck.h"
#include "DeltaPrivate.h"
#include "Listener.h"
#include "Mstp.h"
#include "Zones.h"

using namespace linknet;

namespace {

const char *const kSession = "captures/phase1-usb/20261016-211923";

struct Chunk {
    uint32_t end;      // stream length once this read was appended
    std::string time;  // as written in the .log, six decimals
};

struct Snapshot {
    uint32_t   offset;
    ZoneUpdate update;
    ZoneState  state;
};

struct Marker {
    uint32_t    offset;
    std::string text;
};

std::vector<uint8_t> g_bin;
std::vector<Chunk> g_chunks;
std::vector<Marker> g_markers;
std::vector<std::string> g_rows;  // our CSV, header included
std::map<std::tuple<std::string, int, int>, int> g_counts;
DecoderStats g_decoder;
BusCounts g_check;
std::vector<Snapshot> g_snaps;
uint32_t g_unexpected = 0;
uint32_t g_mismatch = 0;
uint32_t g_notDelta = 0;
support::CaptureLog g_console;

void loadLog() {
    std::ifstream f(support::path(std::string(kSession) + ".log"));
    std::string line;
    uint32_t total = 0;
    while (std::getline(f, line)) {
        // "<time> <offset> <count> <hex>", or "<time> # <marker>"
        const size_t a = line.find(' ');
        if (a == std::string::npos) continue;
        const size_t b = line.find(' ', a + 1);
        if (b == std::string::npos || line.compare(a + 1, b - a - 1, "#") == 0) continue;
        const size_t c = line.find(' ', b + 1);
        if (c == std::string::npos) continue;
        total += static_cast<uint32_t>(std::stoul(line.substr(b + 1, c - b - 1)));
        g_chunks.push_back(Chunk{total, line.substr(0, a)});
    }
}

// The markers were typed in French during the session: DEBUT = start, FIN =
// end, "remise" = back to the reference state. They are kept as recorded.
void loadMarkers() {
    const auto ls = support::lines(support::readText(support::path("captures/phase1-usb/markers.csv")));
    for (size_t i = 1; i < ls.size(); ++i) {
        const std::string &l = ls[i];
        const size_t a = l.find(',');
        const size_t b = l.find(',', a + 1);
        std::string text = l.substr(b + 1);
        if (!text.empty() && text.front() == '"') text = text.substr(1, text.size() - 2);
        g_markers.push_back(Marker{static_cast<uint32_t>(std::stoul(l.substr(0, a))), text});
    }
}

uint32_t marker(const char *needle) {
    for (const Marker &m : g_markers) {
        if (m.text.find(needle) != std::string::npos) return m.offset;
    }
    TEST_FAIL_MESSAGE(needle);
    return 0;
}

class StampedLog : public ILog {
public:
    void write(const char *line) override { g_console.write(line); }
};

void replay() {
    g_bin = support::readBinary(support::path(std::string(kSession) + ".bin"));
    loadLog();
    loadMarkers();

    g_rows.push_back("offset_bin,time,src,dst,service,chaine_octets_hex");
    FrameDecoder dec;
    BusCheck check;
    Zones zones;
    size_t k = 0;
    for (size_t i = 0; i < g_bin.size(); ++i) {
        // A synthetic clock, 130 us per byte: enough for ordering, useless
        // for timing, which is not what this test is about.
        if (!dec.push(g_bin[i], 1000000u + i * 130u)) continue;
        const Frame &f = dec.frame();
        const char *name = frameTypeName(f.type);
        g_counts[std::make_tuple(std::string(name ? name : "?"), f.src, f.dst)]++;

        DeltaMessage m;
        const bool isDelta = f.len > 0 && f.dataCrcOk && parseDelta(f.data, f.len, m);
        check.onFrame(f, isDelta ? &m : nullptr);
        if (f.len > 0 && !isDelta) ++g_notDelta;
        if (!isDelta) continue;

        const uint32_t end = f.offset + f.wireLength();
        while (k < g_chunks.size() && g_chunks[k].end < end) ++k;
        g_rows.push_back(std::to_string(f.offset) + "," +
                         (k < g_chunks.size() ? g_chunks[k].time : "?") + "," +
                         std::to_string(f.src) + "," + std::to_string(f.dst) + "," +
                         std::to_string(m.service) + "," + support::toHex(m.payload, m.len));

        const ZoneUpdate u = zones.apply(f, f.lastByteUs);
        if (u.zone == 0) continue;
        if (u.unexpected) ++g_unexpected;
        if (u.addressMismatch) ++g_mismatch;
        g_snaps.push_back(Snapshot{f.offset, u, *zones.zone(u.zone)});
    }
    g_decoder = dec.stats();
    g_check = check.counts();

    // The same bytes through the firmware's own top level, for its output.
    StampedLog log;
    Listener listener(log);
    for (size_t i = 0; i < g_bin.size(); ++i) {
        listener.push(TimedByte{1000000u + i * 130u, g_bin[i]});
        if (i % 8192 == 0) listener.tick(1000000u + i * 130u, RxHealth());
    }
    listener.printStats(1000000u + g_bin.size() * 130u, RxHealth());
    if (const char *out = std::getenv("LINKNET_REPLAY_OUT")) {
        std::ofstream o(out);
        for (const std::string &l : g_console.lines) o << l << "\n";
    }
}

// The zone state just before `offset`.
const ZoneState *stateAt(uint8_t zone, uint32_t offset) {
    const ZoneState *s = nullptr;
    for (const Snapshot &sn : g_snaps) {
        if (sn.offset >= offset) break;
        if (sn.update.zone == zone) s = &sn.state;
    }
    TEST_ASSERT_NOT_NULL(s);
    return s;
}

std::vector<Key> keysBetween(uint8_t zone, uint32_t from, uint32_t to) {
    std::vector<Key> keys;
    for (const Snapshot &sn : g_snaps) {
        if (sn.offset >= from && sn.offset < to && sn.update.zone == zone && sn.update.keyPressed) {
            keys.push_back(sn.update.key);
        }
    }
    return keys;
}

// The successive setpoints shown on a zone between two offsets, starting
// with the one in force at `from`.
std::string setpointsBetween(uint8_t zone, uint32_t from, uint32_t to) {
    std::string out = stateAt(zone, from)->setpointText;
    for (const Snapshot &sn : g_snaps) {
        if (sn.offset >= from && sn.offset < to && sn.update.zone == zone &&
            (sn.update.changed & kFieldSetpoint)) {
            out += " ";
            out += sn.state.setpointText;
        }
    }
    return out;
}

std::string keyList(const std::vector<Key> &keys) {
    std::string s;
    for (Key k : keys) {
        if (!s.empty()) s += " ";
        s += keyName(k);
    }
    return s;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// --- expected_summary.txt ------------------------------------------------

static void test_summary_matches_reference(void) {
    std::vector<std::string> ours;
    ours.push_back("octets " + std::to_string(g_bin.size()));
    ours.push_back("trames " + std::to_string(g_decoder.frames));
    ours.push_back("crc_donnees_invalides " + std::to_string(g_decoder.dataCrcErrors));
    for (const auto &kv : g_counts) {
        ours.push_back(std::get<0>(kv.first) + " " + std::to_string(std::get<1>(kv.first)) + "->" +
                       std::to_string(std::get<2>(kv.first)) + " " + std::to_string(kv.second));
    }
    const auto ref = support::lines(support::readText(support::path("captures/phase1-usb/expected_summary.txt")));
    TEST_ASSERT_EQUAL_UINT32(ref.size(), ours.size());
    for (size_t i = 0; i < ref.size(); ++i) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(ref[i].c_str(), ours[i].c_str(), ("line " + std::to_string(i + 1)).c_str());
    }
}

static void test_frame_and_crc_counts(void) {
    TEST_ASSERT_EQUAL_UINT32(2305087, g_bin.size());
    TEST_ASSERT_EQUAL_UINT32(284322, g_decoder.frames);
    TEST_ASSERT_EQUAL_UINT32(0, g_decoder.dataCrcErrors);
    TEST_ASSERT_EQUAL_UINT32(0, g_decoder.headerCrcErrors);
    TEST_ASSERT_EQUAL_UINT32(0, g_decoder.badLength);
    TEST_ASSERT_EQUAL_UINT32(0, g_notDelta);  // every data frame is a Delta message
}

// --- expected_delta_messages.csv ---------------------------------------

// One deliberate difference. For service 14 the reference has two bytes
// where the octet string holds three: list_messages.py takes the string as
// data[11:-1], assuming a tag byte plus a length byte (65 LL), but service
// 14 is sent as 63 -- length 3 in the tag byte itself, no length byte
// (ASHRAE 135 clause 20.2.1) -- so its leading 00 was taken for the length.
// docs/report.md section 4 gives the right content, 00 <seq LE>. The comparison
// below drops that leading 00 for service 14 only, and checks it is there.
static void test_messages_match_reference_line_by_line(void) {
    const auto ref =
        support::lines(support::readText(support::path("captures/phase1-usb/expected_delta_messages.csv")));
    TEST_ASSERT_EQUAL_UINT32(ref.size(), g_rows.size());
    TEST_ASSERT_EQUAL_STRING(ref[0].c_str(), g_rows[0].c_str());
    uint32_t service14 = 0;
    for (size_t i = 1; i < ref.size(); ++i) {
        std::string ours = g_rows[i];
        const size_t lastComma = ours.rfind(',');
        const std::string service = ours.substr(ours.rfind(',', lastComma - 1) + 1,
                                                lastComma - ours.rfind(',', lastComma - 1) - 1);
        if (service == "14") {
            TEST_ASSERT_EQUAL_STRING("00", ours.substr(lastComma + 1, 2).c_str());
            ours.erase(lastComma + 1, 2);
            ++service14;
        }
        TEST_ASSERT_EQUAL_STRING_MESSAGE(ref[i].c_str(), ours.c_str(),
                                         ("line " + std::to_string(i + 1)).c_str());
    }
    TEST_ASSERT_EQUAL_UINT32(426, service14);
}

// --- what phase 1 says never happened ---------------------------------

static void test_no_frame_lost_in_the_capture(void) {
    TEST_ASSERT_EQUAL_UINT32(0, g_check.ringBreaks);
    // Token 0->1 sent twice in a row, at offsets 40657, 970513, 1367609 and
    // 1831992 (+8 for the retry). See BusCheck.h.
    TEST_ASSERT_EQUAL_UINT32(4, g_check.tokenRetries);
    TEST_ASSERT_EQUAL_UINT32(0, g_check.seqJumps);
    TEST_ASSERT_EQUAL_UINT32(0, g_check.ackMismatch);
    TEST_ASSERT_EQUAL_UINT32(0, g_check.unacked);
    // Only the tail of the frame cut by the start of the recording:
    // 08 48 e0 00 01 00 00 00 73, then the first 55 FF.
    TEST_ASSERT_EQUAL_UINT32(9, g_decoder.bytesOutsideFrames());
}

static void test_no_unexpected_bytes(void) {
    TEST_ASSERT_EQUAL_UINT32(0, g_unexpected);
    TEST_ASSERT_EQUAL_UINT32(0, g_mismatch);
}

// --- the tests of phase 1, at their markers ----------------------------

static void test_state_before_test_2(void) {
    const uint32_t at = marker("DEBUT test 2");
    TEST_ASSERT_EQUAL_STRING("20.0", stateAt(1, at)->setpointText);
    TEST_ASSERT_EQUAL_STRING("20.0", stateAt(2, at)->setpointText);
}

static void test_2_bedroom_up_three_times(void) {
    const uint32_t from = marker("DEBUT test 2"), to = marker("FIN test 2");
    TEST_ASSERT_EQUAL_STRING("UP UP UP", keyList(keysBetween(1, from, to)).c_str());
    TEST_ASSERT_EQUAL_STRING("", keyList(keysBetween(2, from, to)).c_str());
    TEST_ASSERT_EQUAL_STRING("20.0 20.1 20.2 20.3", setpointsBetween(1, from, to).c_str());
    TEST_ASSERT_EQUAL_STRING("20.3", stateAt(1, to)->setpointText);  // "FIN test 2. T1 csg 20.3"
    TEST_ASSERT_EQUAL_STRING("20.0", stateAt(2, to)->setpointText);
}

static void test_3_bedroom_down_three_times(void) {
    const uint32_t from = marker("DEBUT test 3"), to = marker("FIN test 3");
    TEST_ASSERT_EQUAL_STRING("DOWN DOWN DOWN", keyList(keysBetween(1, from, to)).c_str());
    TEST_ASSERT_EQUAL_STRING("20.3 20.2 20.1 20.0", setpointsBetween(1, from, to).c_str());
    TEST_ASSERT_EQUAL_STRING("20.0", stateAt(1, to)->setpointText);
}

static void test_4a_living_up_three_times(void) {
    const uint32_t from = marker("DEBUT test 4a"), to = marker("FIN test 4a");
    TEST_ASSERT_EQUAL_STRING("UP UP UP", keyList(keysBetween(2, from, to)).c_str());
    TEST_ASSERT_EQUAL_STRING("", keyList(keysBetween(1, from, to)).c_str());
    TEST_ASSERT_EQUAL_STRING("20.0 20.1 20.2 20.3", setpointsBetween(2, from, to).c_str());
    TEST_ASSERT_EQUAL_STRING("20.3", stateAt(2, to)->setpointText);
    TEST_ASSERT_EQUAL_STRING("20.0", stateAt(1, to)->setpointText);  // the other zone untouched
}

static void test_4b_living_down_three_times(void) {
    const uint32_t from = marker("DEBUT test 4b"), to = marker("FIN test 4b");
    TEST_ASSERT_EQUAL_STRING("DOWN DOWN DOWN", keyList(keysBetween(2, from, to)).c_str());
    TEST_ASSERT_EQUAL_STRING("20.3 20.2 20.1 20.0", setpointsBetween(2, from, to).c_str());
    TEST_ASSERT_EQUAL_STRING("20.0", stateAt(2, to)->setpointText);
}

// docs/report.md section 4 says raw 2208 -> 2182 while the display rose 21.1 ->
// 21.8. The decoded sequence starts one step earlier: 2213 at 21.1, which
// makes it 31 units for 0.7 degrees, about -44 per degree rather than -40.
static void test_5_hand_on_bedroom_sensor(void) {
    const uint32_t from = marker("DEBUT test 5"), to = marker("FIN test 5");
    std::string raws, indoors;
    for (const Snapshot &sn : g_snaps) {
        if (sn.offset < from || sn.offset >= to || sn.update.zone != 1) continue;
        if (sn.update.changed & kFieldRaw) raws += (raws.empty() ? "" : " ") + std::to_string(sn.state.raw);
        if (sn.update.changed & kFieldIndoor) indoors += (indoors.empty() ? "" : " ") + std::string(sn.state.indoorText);
    }
    TEST_ASSERT_EQUAL_STRING("2213 2208 2203 2197 2192 2187 2182", raws.c_str());
    TEST_ASSERT_EQUAL_STRING("21.1 21.2 21.3 21.4 21.5 21.6 21.7 21.8", indoors.c_str());
    TEST_ASSERT_EQUAL_STRING("21.8", stateAt(1, to)->indoorText);  // "FIN test 5. T1 int 21.8"
    TEST_ASSERT_EQUAL_STRING("", keyList(keysBetween(1, from, to)).c_str());
}

static void test_6a_living_off(void) {
    const uint32_t from = marker("DEBUT test 6a"), to = marker("FIN test 6a");
    TEST_ASSERT_EQUAL_STRING("OFF", keyList(keysBetween(2, from, to)).c_str());
    TEST_ASSERT_TRUE(stateAt(2, from)->on);
    TEST_ASSERT_TRUE(stateAt(2, from)->fan);
    TEST_ASSERT_FALSE(stateAt(2, to)->on);
    TEST_ASSERT_FALSE(stateAt(2, to)->fan);  // marker: fan icon gone
}

static void test_6b_living_on(void) {
    const uint32_t from = marker("DEBUT test 6b"), to = marker("FIN test 6b");
    TEST_ASSERT_EQUAL_STRING("ON", keyList(keysBetween(2, from, to)).c_str());
    TEST_ASSERT_TRUE(stateAt(2, to)->on);    // marker: ON shown
    TEST_ASSERT_FALSE(stateAt(2, to)->fan);  // marker: no fan icon
}

// docs/report.md section 6: the bedroom sends ON and OFF, the controller ignores them.
static void test_6c_6d_bedroom_on_off_ignored(void) {
    const uint32_t c0 = marker("DEBUT test 6c"), c1 = marker("FIN test 6c");
    const uint32_t d0 = marker("DEBUT test 6d"), d1 = marker("FIN test 6d");
    TEST_ASSERT_EQUAL_STRING("ON", keyList(keysBetween(1, c0, c1)).c_str());
    TEST_ASSERT_EQUAL_STRING("OFF", keyList(keysBetween(1, d0, d1)).c_str());
    for (uint32_t at : {c0, c1, d0, d1}) {
        TEST_ASSERT_FALSE(stateAt(1, at)->on);
        TEST_ASSERT_FALSE(stateAt(1, at)->fan);
    }
}

// docs/report.md section 6: the fan came back when the setpoint reached 19.4.
static void test_7_living_down_until_the_fan_returns(void) {
    const uint32_t from = marker("DEBUT test 7"), to = marker("FIN test 7");
    const std::vector<Key> keys = keysBetween(2, from, to);
    TEST_ASSERT_EQUAL_STRING("DOWN DOWN DOWN DOWN DOWN DOWN DOWN", keyList(keys).c_str());
    TEST_ASSERT_EQUAL_STRING("19.3", stateAt(2, to)->setpointText);  // "T2 csg 19.3"
    TEST_ASSERT_FALSE(stateAt(2, from)->fan);
    TEST_ASSERT_TRUE(stateAt(2, to)->fan);  // marker: fan back
    const char *fanAt = nullptr;
    for (const Snapshot &sn : g_snaps) {
        if (sn.offset >= from && sn.offset < to && sn.update.zone == 2 &&
            (sn.update.changed & kFieldFan) && sn.state.fan) {
            fanAt = sn.state.setpointText;
            break;
        }
    }
    TEST_ASSERT_NOT_NULL(fanAt);
    TEST_ASSERT_EQUAL_STRING("19.4", fanAt);
}

static void test_restore_living_up_seven_times(void) {
    const uint32_t from = marker("DEBUT remise"), to = marker("FIN remise");
    TEST_ASSERT_EQUAL_STRING("UP UP UP UP UP UP UP", keyList(keysBetween(2, from, to)).c_str());
    TEST_ASSERT_EQUAL_STRING("20.0", stateAt(2, to)->setpointText);
    TEST_ASSERT_TRUE(stateAt(2, to)->fan);  // marker: fan still shown
}

// The reference state (docs/report.md section 6), at the end of the recording.
static void test_final_state_is_the_reference_state(void) {
    const uint32_t end = static_cast<uint32_t>(g_bin.size());
    const ZoneState *bedroom = stateAt(1, end);
    const ZoneState *living = stateAt(2, end);
    TEST_ASSERT_EQUAL_STRING("20.0", bedroom->setpointText);
    TEST_ASSERT_EQUAL_STRING("20.0", living->setpointText);
    TEST_ASSERT_TRUE(living->fan);
    TEST_ASSERT_TRUE(living->on);
    TEST_ASSERT_FALSE(bedroom->fan);
    TEST_ASSERT_FALSE(bedroom->on);
}

// --- the firmware's console over the capture --------------------------

static void test_console_output(void) {
    TEST_ASSERT_TRUE(g_console.any("STATS frames=284322 "));
    TEST_ASSERT_TRUE(g_console.any(" hdr_crc=0 bad_len=0 data_crc=0 "));
    TEST_ASSERT_TRUE(g_console.any(" ring_breaks=0 token_retries=4 seq_jumps=0 seq_missing=0 "));
    TEST_ASSERT_EQUAL_UINT32(4, g_console.count(" RETRY Token 0->1, "));
    TEST_ASSERT_FALSE(g_console.any("ODD"));
    TEST_ASSERT_FALSE(g_console.any("ERR"));
    // 3 + 3 + 3 + 3 + 1 + 1 + 1 + 1 + 7 + 7 key presses in the tests above
    TEST_ASSERT_EQUAL_UINT32(30, g_console.count(" key="));
    TEST_ASSERT_TRUE(g_console.any("Z1 bedroom in="));
    TEST_ASSERT_TRUE(g_console.any("Z2 living in="));
}

int main(int, char **) {
    replay();
    UNITY_BEGIN();
    RUN_TEST(test_summary_matches_reference);
    RUN_TEST(test_frame_and_crc_counts);
    RUN_TEST(test_messages_match_reference_line_by_line);
    RUN_TEST(test_no_frame_lost_in_the_capture);
    RUN_TEST(test_no_unexpected_bytes);
    RUN_TEST(test_state_before_test_2);
    RUN_TEST(test_2_bedroom_up_three_times);
    RUN_TEST(test_3_bedroom_down_three_times);
    RUN_TEST(test_4a_living_up_three_times);
    RUN_TEST(test_4b_living_down_three_times);
    RUN_TEST(test_5_hand_on_bedroom_sensor);
    RUN_TEST(test_6a_living_off);
    RUN_TEST(test_6b_living_on);
    RUN_TEST(test_6c_6d_bedroom_on_off_ignored);
    RUN_TEST(test_7_living_down_until_the_fan_returns);
    RUN_TEST(test_restore_living_up_seven_times);
    RUN_TEST(test_final_state_is_the_reference_state);
    RUN_TEST(test_console_output);
    return UNITY_END();
}
