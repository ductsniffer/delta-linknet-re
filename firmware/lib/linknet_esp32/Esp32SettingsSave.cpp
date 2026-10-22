// The NVS writers, kept out of the bus images (see Esp32Settings.h).
#include <Preferences.h>

#include <cstring>

#include "Esp32Settings.h"

namespace linknet {
namespace esp32 {

bool saveNetSettings(const NetSettings &s) {
    Preferences p;
    if (!p.begin("linknet", false)) return false;
    // putString() returns the length written, 0 on failure -- and also for
    // an empty value, which an open network's passphrase legitimately is.
    bool ok = p.putString("ssid", s.ssid) == strlen(s.ssid);
    ok = p.putString("pass", s.pass) == strlen(s.pass) && ok;
    ok = p.putString("ota", s.otaPass) == strlen(s.otaPass) && ok;
    p.end();
    return ok;
}

bool eraseNetSettings() {
    Preferences p;
    if (!p.begin("linknet", false)) return false;
    const bool ok = p.clear();
    p.end();
    return ok;
}

}  // namespace esp32
}  // namespace linknet
