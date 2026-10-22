#include "Esp32Settings.h"

#include <Preferences.h>

namespace linknet {
namespace esp32 {

bool loadNetSettings(NetSettings &s) {
    s = NetSettings{};
    Preferences p;
    // Read-only: fails, rather than creating the namespace, when the board
    // was never provisioned.
    if (!p.begin("linknet", true)) return false;
    p.getString("ssid", s.ssid, sizeof s.ssid);
    p.getString("pass", s.pass, sizeof s.pass);
    p.getString("ota", s.otaPass, sizeof s.otaPass);
    p.end();
    return s.ssid[0] != '\0';
}

}  // namespace esp32
}  // namespace linknet
