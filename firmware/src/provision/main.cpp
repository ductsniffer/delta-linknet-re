// The provisioning image: writes the Wi-Fi settings into NVS, and does
// nothing else (lib/linknet_core/NetSettings.h says why they are not
// compiled in).
//
//   ./flash.ps1 -Port COM6 -Esptool ... -Target provision
//   ./provision.ps1 -Port COM6          asks for the network and its
//                                       passphrase, sends them, saves, tests
//   ./flash.ps1 -Port COM6 -Esptool ... -Target edges    (or the listener)
//
// NVS survives both flashes and every update over Wi-Fi.
//
// It does not touch the bus pins: no UART but UART0 (the console), no
// GPIO 17 or 18. The one thing that can go wrong with it is a board left
// running it, which listens to nothing; so it says what it is at boot and
// blinks magenta, a colour the bus images never show.
#include <Arduino.h>
#include <WiFi.h>

#include <cstring>

#include "BoardConfig.h"
#include "Esp32Adapters.h"
#include "Esp32Settings.h"
#include "NetSettings.h"

using namespace linknet;

namespace {

esp32::SerialLog   g_log;
esp32::NeoPixelLed g_led(24);
NetSettings        g_settings;

// "ota " + 64 characters in hex is the longest line: 132 bytes.
char   g_line[160];
size_t g_lineLen = 0;
bool   g_lineTooLong = false;

const char *statusName(wl_status_t s) {
    switch (s) {
        case WL_NO_SSID_AVAIL: return "network not found";
        case WL_CONNECT_FAILED: return "connection refused, wrong passphrase?";
        case WL_CONNECTION_LOST: return "connection lost";
        case WL_DISCONNECTED: return "disconnected, wrong passphrase?";
        case WL_IDLE_STATUS: return "idle";
        default: return "other";
    }
}

void show(const char *what) {
    char d[128];
    describeSettings(g_settings, d, sizeof d);
    g_log.logf("%s %s", what, d);
}

// Joins with the settings as they stand, saved or not, and leaves again.
void testWifi() {
    g_log.write("WIFI testing (20 s at most)...");
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_settings.ssid, g_settings.pass);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(100);
    const wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) {
        g_log.logf("WIFI OK ip=%s rssi=%d dBm channel=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                   WiFi.channel());
    } else {
        g_log.logf("WIFI FAIL status=%d (%s)", static_cast<int>(s), statusName(s));
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
}

void handle(const char *line) {
    const ProvisionReply r = provisionLine(line, g_settings);
    switch (r.action) {
        case ProvisionAction::None: break;
        case ProvisionAction::Updated:
        case ProvisionAction::Error: g_log.write(r.message); break;
        case ProvisionAction::Show: show("SETTINGS"); break;
        case ProvisionAction::Save:
            if (esp32::saveNetSettings(g_settings)) {
                show("SAVED");
            } else {
                g_log.write("ERR save: NVS write failed");
            }
            break;
        case ProvisionAction::Test: testWifi(); break;
        case ProvisionAction::Erase:
            g_log.write(esp32::eraseNetSettings() ? "ERASED" : "ERR erase: NVS write failed");
            break;
    }
}

}  // namespace

void setup() {
    Serial.begin(board::kConsoleBaud);
    g_led.begin();
    delay(100);
    g_log.write("");
    g_log.write("LINKnet PROVISIONING image, built " __DATE__ " " __TIME__);
    g_log.write("Writes the Wi-Fi settings to NVS. Does NOT listen to the bus: flash a bus image after.");
    g_log.write("commands: ssid <hex> | pass <hex> | ota <hex> | show | save | test | erase");
    if (esp32::loadNetSettings(g_settings)) {
        show("SETTINGS (saved)");
    } else {
        g_log.write("SETTINGS none saved yet");
    }
    g_log.write("READY");
}

void loop() {
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c == '\r' || c == '\n') {
            g_line[g_lineLen] = '\0';
            if (g_lineTooLong) {
                g_log.write("ERR line too long");
            } else if (g_lineLen > 0) {
                handle(g_line);
            }
            g_lineLen = 0;
            g_lineTooLong = false;
        } else if (g_lineLen + 1 < sizeof g_line) {
            g_line[g_lineLen++] = static_cast<char>(c);
        } else {
            g_lineTooLong = true;
        }
    }
    g_led.show((millis() / 125) % 2 ? Rgb{255, 0, 255} : Rgb{});
    delay(5);
}
