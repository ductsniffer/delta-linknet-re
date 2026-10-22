// NetSettings in NVS: namespace "linknet", keys "ssid", "pass", "ota".
//
// Split in two files on purpose: the bus images (listener, edge counter)
// call loadNetSettings() only, so the archive member holding the writers,
// Esp32SettingsSave.cpp, is never linked into them. Only src/provision/
// main.cpp writes NVS (NetSettings.h explains why).
#pragma once

#include "NetSettings.h"

namespace linknet {
namespace esp32 {

// Opens the namespace read-only. False when nothing was ever saved.
bool loadNetSettings(NetSettings &s);

// Provisioning image only.
bool saveNetSettings(const NetSettings &s);
bool eraseNetSettings();

}  // namespace esp32
}  // namespace linknet
