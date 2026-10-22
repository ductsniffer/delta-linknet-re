// The Wi-Fi settings of a board, and the console that writes them.
//
// WHY NOT COMPILED IN
// -------------------
// As in an earlier project of ours: the network name, its
// passphrase and the update password live in the board's NVS, written once
// over the USB cable by a separate image (src/provision/main.cpp, driven by
// provision.ps1). They are never in the source tree, in a build or in a log,
// and an image updated over Wi-Fi keeps them: NVS is its own partition
// (0x9000 in default_8MB.csv), which neither esptool's images nor an OTA
// update touches.
//
// The bus images (listener, edge counter) only ever load them. Only the
// provisioning image saves them.
#pragma once

#include <cstddef>
#include <cstdint>

namespace linknet {

struct NetSettings {
    // 802.11: an SSID is at most 32 octets. WPA2: a passphrase is 8 to 63
    // characters, or exactly 64 hex digits (the PSK itself). +1 for the NUL.
    static constexpr size_t kSsidCap = 33;
    static constexpr size_t kPassCap = 65;
    static constexpr size_t kOtaCap = 65;
    // provision.ps1 generates 24 characters; anything much shorter is a typo.
    static constexpr size_t kOtaMin = 12;

    char ssid[kSsidCap] = {};
    char pass[kPassCap] = {};     // empty: an open network
    char otaPass[kOtaCap] = {};   // HTTP basic auth on POST /update, user "linknet"

    // Enough to join a network and to refuse updates from strangers.
    bool complete() const;
};

// Hex text to a NUL-terminated string. The provisioning lines carry every
// value in hex, so a space, a quote or a non-ASCII letter in a network name
// needs no escaping between PowerShell, the serial port and this parser.
// Fails on odd length, a non-hex digit, an embedded NUL, or no room.
bool decodeHexText(const char *hex, char *out, size_t cap);

// One line of the provisioning console:
//   ssid <hex>   pass <hex>   ota <hex>      set a value (pass may be empty)
//   show   save   test   erase               reported back to the caller
// Replies start with OK or ERR; the passphrases are never echoed, only
// their length.
enum class ProvisionAction { None, Updated, Show, Save, Test, Erase, Error };

struct ProvisionReply {
    ProvisionAction action = ProvisionAction::None;
    char            message[96] = {};
};

ProvisionReply provisionLine(const char *line, NetSettings &s);

// "ssid=<name> pass=<n> chars ota=<n> chars", for show and save.
void describeSettings(const NetSettings &s, char *out, size_t cap);

}  // namespace linknet
