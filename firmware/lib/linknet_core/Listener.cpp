#include "Listener.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace linknet {

namespace {

// Seconds since boot, to the millisecond: "   1234.567".
struct Stamp {
    char s[24];
    explicit Stamp(uint64_t us) {
        snprintf(s, sizeof s, "%7lu.%03lu", static_cast<unsigned long>(us / 1000000u),
                 static_cast<unsigned long>(us / 1000u % 1000u));
    }
};

void appendf(char *buf, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void appendf(char *buf, size_t cap, const char *fmt, ...) {
    const size_t used = strlen(buf);
    if (used + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);
}

void appendHex(char *buf, size_t cap, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i) appendf(buf, cap, "%02x", p[i]);
}

void appendMask(char *buf, size_t cap, uint64_t mask) {
    bool first = true;
    for (unsigned i = 0; i < 64; ++i) {
        if (!(mask & (uint64_t{1} << i))) continue;
        appendf(buf, cap, first ? "%u" : ",%u", i);
        first = false;
    }
}

const char *text(bool have, const char *t) { return have ? t : "?"; }

}  // namespace

Listener::Listener(ILog &log, const ListenerConfig &cfg)
    : log_(log), cfg_(cfg), timing_(cfg.baud) {}

void Listener::poll(IByteSource &src) {
    TimedByte buf[128];
    size_t n;
    while ((n = src.read(buf, sizeof buf / sizeof buf[0])) > 0) {
        for (size_t i = 0; i < n; ++i) push(buf[i]);
    }
}

void Listener::push(const TimedByte &b) {
    if (decoder_.push(b)) onFrame(decoder_.frame());
}

void Listener::noteErrors(uint64_t tUs) {
    const DecoderStats &d = decoder_.stats();
    const BusCounts &c = check_.counts();
    const uint32_t total = d.headerCrcErrors + d.badLength + d.dataCrcErrors + c.ringBreaks +
                           c.seqJumps + c.ackMismatch;
    if (total != errorTotal_) {
        errorTotal_ = total;
        haveError_ = true;
        lastErrorUs_ = tUs;
    }
}

void Listener::printZone(uint64_t tUs, uint8_t zone, const char *suffix) {
    const ZoneState *z = zones_.zone(zone);
    if (z == nullptr) return;
    char raw[8];
    snprintf(raw, sizeof raw, "%u", static_cast<unsigned>(z->raw));
    const bool d = z->haveDisplay;
    log_.logf("%s Z%u %s in=%s sp=%s out=%s fan=%s on=%s adj=%s raw=%s%s", Stamp(tUs).s,
              static_cast<unsigned>(zone), zoneName(zone), text(d, z->indoorText),
              text(d, z->setpointText), text(d, z->outdoorText), d ? (z->fan ? "1" : "0") : "?",
              d ? (z->on ? "1" : "0") : "?", d ? (z->adjusting ? "1" : "0") : "?",
              text(z->haveSensor, raw), suffix);
}

void Listener::onFrame(const Frame &f) {
    DeltaMessage m;
    const bool isDelta = f.len > 0 && f.dataCrcOk && parseDelta(f.data, f.len, m);
    check_.onFrame(f, isDelta ? &m : nullptr);
    timing_.onFrame(f, isDelta ? &m : nullptr);

    if (silent_) {
        silent_ = false;
        log_.logf("%s BUS back after %lu ms of silence", Stamp(f.lastByteUs).s,
                  static_cast<unsigned long>((f.firstByteUs - lastFrameUs_) / 1000u));
    }
    haveFrame_ = true;
    lastFrameUs_ = f.lastByteUs;
    noteErrors(f.lastByteUs);
    if (check_.lastWasRetry()) {
        // Rare (4 in the 28 minutes of phase 1) and exactly the delay a
        // future transmitting station must stay under, so each one is shown.
        log_.logf("%s RETRY %s %u->%u, %lu us after the unanswered token", Stamp(f.lastByteUs).s,
                  frameTypeName(f.type) ? frameTypeName(f.type) : "?",
                  static_cast<unsigned>(f.src), static_cast<unsigned>(f.dst),
                  static_cast<unsigned long>(timing_.lastGapUs()));
    }
    if (!f.dataCrcOk) {
        log_.logf("%s ERR data CRC %u->%u type %u len %u", Stamp(f.lastByteUs).s,
                  static_cast<unsigned>(f.src), static_cast<unsigned>(f.dst),
                  static_cast<unsigned>(f.type), static_cast<unsigned>(f.len));
    }
    if (!isDelta) return;

    const ZoneUpdate u = zones_.apply(f, f.lastByteUs);
    if (u.zone == 0) return;
    const Stamp at(f.lastByteUs);

    if (u.changed) {
        static const struct {
            uint16_t bit;
            const char *name;
        } kNames[] = {{kFieldFirst, "first"}, {kFieldIndoor, "in"},  {kFieldSetpoint, "sp"},
                      {kFieldOutdoor, "out"}, {kFieldFan, "fan"},    {kFieldOn, "on"},
                      {kFieldAdjusting, "adj"}, {kFieldRaw, "raw"}};
        char suffix[64] = " (";
        bool first = true;
        for (const auto &n : kNames) {
            if (!(u.changed & n.bit)) continue;
            appendf(suffix, sizeof suffix, first ? "%s" : " %s", n.name);
            first = false;
        }
        appendf(suffix, sizeof suffix, ")");
        printZone(f.lastByteUs, u.zone, suffix);
    }
    if (u.keyPressed) {
        const ZoneState *z = zones_.zone(u.zone);
        log_.logf("%s Z%u %s key=%s mask=%02x raw=%u", at.s, static_cast<unsigned>(u.zone),
                  zoneName(u.zone), keyName(u.key), static_cast<unsigned>(u.keyMask),
                  static_cast<unsigned>(z->raw));
    }
    if (u.addressMismatch) {
        log_.logf("%s Z%u ODD service 12 from address %u names zone %u in byte 1", at.s,
                  static_cast<unsigned>(u.zone), static_cast<unsigned>(f.src),
                  static_cast<unsigned>(m.payload[1]));
    }
    const int which = u.service == static_cast<uint8_t>(Service::Display) ? 1 : 0;
    uint64_t &reported = oddReported_[u.zone][which];
    if (u.unexpected != reported) {
        char line[320];
        snprintf(line, sizeof line, "%s Z%u ODD service %u ", at.s,
                 static_cast<unsigned>(u.zone), static_cast<unsigned>(u.service));
        if (u.unexpected) {
            appendf(line, sizeof line, "unusual bytes ");
            appendMask(line, sizeof line, u.unexpected);
            appendf(line, sizeof line, ": ");
            appendHex(line, sizeof line, m.payload, m.len);
        } else {
            appendf(line, sizeof line, "back to the usual bytes");
        }
        log_.write(line);
        reported = u.unexpected;
    }
}

void Listener::tick(uint64_t nowUs, const RxHealth &rx) {
    const uint32_t rxErrors =
        rx.fifoOverflows + rx.ringOverflows + rx.framingErrors + rx.parityErrors + rx.breaks;
    if (rxErrors != rxErrorTotal_) {
        rxErrorTotal_ = rxErrors;
        haveError_ = true;
        lastErrorUs_ = nowUs;
    }
    if (haveFrame_ && !silent_ && nowUs > lastFrameUs_ &&
        nowUs - lastFrameUs_ > uint64_t{cfg_.silentMs} * 1000u) {
        silent_ = true;
        log_.logf("%s BUS silent: no frame for %lu ms", Stamp(nowUs).s,
                  static_cast<unsigned long>(cfg_.silentMs));
    }
    const uint64_t period = uint64_t{cfg_.statsPeriodS} * 1000000u;
    if (nextStatsUs_ == 0) nextStatsUs_ = nowUs + period;
    if (nowUs >= nextStatsUs_) {
        printStats(nowUs, rx);
        while (nextStatsUs_ <= nowUs) nextStatsUs_ += period;
    }
}

void Listener::printStats(uint64_t nowUs, const RxHealth &rx) {
    const Stamp at(nowUs);
    const DecoderStats &d = decoder_.stats();
    const BusCounts &c = check_.counts();
    log_.logf("%s STATS frames=%lu token=%lu pfm=%lu data=%lu hdr_crc=%lu bad_len=%lu "
              "data_crc=%lu junk_bytes=%lu ring_breaks=%lu token_retries=%lu seq_jumps=%lu "
              "seq_missing=%lu acked=%lu unacked=%lu ack_mismatch=%lu",
              at.s, static_cast<unsigned long>(d.frames),
              static_cast<unsigned long>(c.byType[static_cast<int>(FrameType::Token)]),
              static_cast<unsigned long>(c.byType[static_cast<int>(FrameType::PollForMaster)]),
              static_cast<unsigned long>(c.byType[static_cast<int>(FrameType::DataNotExpectingReply)] +
                                         c.byType[static_cast<int>(FrameType::DataExpectingReply)]),
              static_cast<unsigned long>(d.headerCrcErrors), static_cast<unsigned long>(d.badLength),
              static_cast<unsigned long>(d.dataCrcErrors),
              static_cast<unsigned long>(d.bytesOutsideFrames()),
              static_cast<unsigned long>(c.ringBreaks), static_cast<unsigned long>(c.tokenRetries),
              static_cast<unsigned long>(c.seqJumps),
              static_cast<unsigned long>(c.seqMissing), static_cast<unsigned long>(c.acked),
              static_cast<unsigned long>(c.unacked), static_cast<unsigned long>(c.ackMismatch));
    log_.logf("%s UART bytes=%lu irq=%lu multi=%lu timeout=%lu fifo_ovf=%lu ring_ovf=%lu "
              "framing=%lu parity=%lu break=%lu",
              at.s, static_cast<unsigned long>(d.bytes), static_cast<unsigned long>(rx.interrupts),
              static_cast<unsigned long>(rx.multiByte), static_cast<unsigned long>(rx.timeouts),
              static_cast<unsigned long>(rx.fifoOverflows),
              static_cast<unsigned long>(rx.ringOverflows),
              static_cast<unsigned long>(rx.framingErrors),
              static_cast<unsigned long>(rx.parityErrors), static_cast<unsigned long>(rx.breaks));
    for (uint8_t zone = 1; zone <= kZoneCount; ++zone) {
        const ZoneState *z = zones_.zone(zone);
        char suffix[96];
        if (z->haveDisplay) {
            const uint64_t ageMs = nowUs > z->lastDisplayUs ? (nowUs - z->lastDisplayUs) / 1000u : 0;
            snprintf(suffix, sizeof suffix, " last13=%lu.%lus n13=%lu n12=%lu keys=%lu%s",
                     static_cast<unsigned long>(ageMs / 1000u),
                     static_cast<unsigned long>(ageMs / 100u % 10u),
                     static_cast<unsigned long>(z->displays), static_cast<unsigned long>(z->sensors),
                     static_cast<unsigned long>(z->keys),
                     ageMs > uint64_t{cfg_.staleS} * 1000u ? " STALE" : "");
        } else {
            snprintf(suffix, sizeof suffix, " last13=never n12=%lu keys=%lu STALE",
                     static_cast<unsigned long>(z->sensors), static_cast<unsigned long>(z->keys));
        }
        printZone(nowUs, zone, suffix);
    }
    for (int g = 0; g < static_cast<int>(Gap::kCount); ++g) {
        const GapStats &s = timing_.stats(static_cast<Gap>(g));
        if (s.count() == 0) continue;
        log_.logf("%s GAP %-11s n=%lu min=%lu p50=%lu p99=%lu max=%lu avg=%lu us", at.s,
                  gapName(static_cast<Gap>(g)), static_cast<unsigned long>(s.count()),
                  static_cast<unsigned long>(s.min()), static_cast<unsigned long>(s.quantile(0.5)),
                  static_cast<unsigned long>(s.quantile(0.99)), static_cast<unsigned long>(s.max()),
                  static_cast<unsigned long>(s.mean()));
    }
}

Rgb Listener::status(uint64_t nowUs) const {
    if (!haveFrame_) return Rgb{0, 0, 255};
    if (nowUs > lastFrameUs_ && nowUs - lastFrameUs_ > uint64_t{cfg_.silentMs} * 1000u) {
        return Rgb{255, 0, 0};
    }
    if (haveError_ && nowUs - lastErrorUs_ < uint64_t{cfg_.errorHoldS} * 1000000u) {
        return Rgb{255, 110, 0};
    }
    return Rgb{0, 255, 0};
}

}  // namespace linknet
