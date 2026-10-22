#include "NetSettings.h"

#include <cstdio>
#include <cstring>

namespace linknet {

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// "ssid 4142" -> word "ssid", argument "4142". The argument may be absent.
bool splitCommand(const char *line, char *word, size_t wordCap, const char **arg) {
    while (*line == ' ') ++line;
    size_t n = 0;
    while (line[n] && !blank(line[n])) ++n;
    if (n == 0 || n >= wordCap) return false;
    memcpy(word, line, n);
    word[n] = '\0';
    const char *rest = line + n;
    while (*rest == ' ') ++rest;
    *arg = rest;
    return true;
}

// Trailing blanks (PuTTY or a script may leave a '\r') are not hex digits.
size_t trimmedLength(const char *s) {
    size_t n = strlen(s);
    while (n > 0 && blank(s[n - 1])) --n;
    return n;
}

void reply(ProvisionReply &r, ProvisionAction a, const char *fmt, size_t value = 0) {
    r.action = a;
    snprintf(r.message, sizeof r.message, fmt, static_cast<unsigned>(value));
}

}  // namespace

bool NetSettings::complete() const { return ssid[0] != '\0' && strlen(otaPass) >= kOtaMin; }

bool decodeHexText(const char *hex, char *out, size_t cap) {
    const size_t len = trimmedLength(hex);
    if (len % 2 != 0 || len / 2 + 1 > cap) return false;
    for (size_t i = 0; i < len; i += 2) {
        const int hi = hexDigit(hex[i]);
        const int lo = hexDigit(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        const char c = static_cast<char>(hi << 4 | lo);
        if (c == '\0') return false;
        out[i / 2] = c;
    }
    out[len / 2] = '\0';
    return true;
}

ProvisionReply provisionLine(const char *line, NetSettings &s) {
    ProvisionReply r;
    char word[8];
    const char *arg = nullptr;
    if (trimmedLength(line) == 0) return r;
    if (!splitCommand(line, word, sizeof word, &arg)) {
        reply(r, ProvisionAction::Error, "ERR unknown command");
        return r;
    }

    if (strcmp(word, "ssid") == 0) {
        char v[NetSettings::kSsidCap];
        if (!decodeHexText(arg, v, sizeof v) || v[0] == '\0') {
            reply(r, ProvisionAction::Error, "ERR ssid: 1 to 32 bytes, in hex");
            return r;
        }
        memcpy(s.ssid, v, sizeof v);
        reply(r, ProvisionAction::Updated, "OK ssid set (%u bytes)", strlen(v));
    } else if (strcmp(word, "pass") == 0) {
        char v[NetSettings::kPassCap];
        const size_t n = decodeHexText(arg, v, sizeof v) ? strlen(v) : 1;
        if (n != 0 && (n < 8 || n > 64)) {
            reply(r, ProvisionAction::Error, "ERR pass: empty, or 8 to 64 characters, in hex");
            return r;
        }
        memcpy(s.pass, v, sizeof v);
        reply(r, ProvisionAction::Updated, "OK pass set (%u chars)", n);
    } else if (strcmp(word, "ota") == 0) {
        char v[NetSettings::kOtaCap];
        if (!decodeHexText(arg, v, sizeof v) || strlen(v) < NetSettings::kOtaMin) {
            reply(r, ProvisionAction::Error, "ERR ota: 12 to 64 characters, in hex");
            return r;
        }
        memcpy(s.otaPass, v, sizeof v);
        reply(r, ProvisionAction::Updated, "OK ota set (%u chars)", strlen(v));
    } else if (strcmp(word, "show") == 0) {
        r.action = ProvisionAction::Show;
    } else if (strcmp(word, "save") == 0) {
        if (!s.complete()) {
            reply(r, ProvisionAction::Error, "ERR save: ssid and ota are needed first");
            return r;
        }
        r.action = ProvisionAction::Save;
    } else if (strcmp(word, "test") == 0) {
        if (s.ssid[0] == '\0') {
            reply(r, ProvisionAction::Error, "ERR test: no ssid");
            return r;
        }
        r.action = ProvisionAction::Test;
    } else if (strcmp(word, "erase") == 0) {
        s = NetSettings{};
        r.action = ProvisionAction::Erase;
    } else {
        reply(r, ProvisionAction::Error, "ERR unknown command");
    }
    return r;
}

void describeSettings(const NetSettings &s, char *out, size_t cap) {
    snprintf(out, cap, "ssid=%s pass=%u chars ota=%u chars", s.ssid[0] ? s.ssid : "(none)",
             static_cast<unsigned>(strlen(s.pass)), static_cast<unsigned>(strlen(s.otaPass)));
}

}  // namespace linknet
