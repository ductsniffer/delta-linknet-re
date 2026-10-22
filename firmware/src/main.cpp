// LINKnet listener, ESP32-S3: decodes the thermostat bus and prints, per
// zone, what the thermostat shows and measures, plus the bus statistics and
// inter-frame delays. It transmits nothing, by construction:
//
//   - the RS-485 module's RXD (its transmit input) is tied to its VCC;
//   - UART1 is configured with a receive pin only (Esp32RxUart.h), and
//     nothing in the firmware writes to it.
//
// Everything that decides what to print lives in lib/linknet_core and runs
// natively in the tests, replaying the phase 1 capture. This file wires it
// to the hardware.
//
// The console goes out on UART0 and, once the board is on the Wi-Fi, on
// tcp/23 (Esp32Net.h): during the tests the board is by thermostat 1, on a
// charger, out of reach of the laptop's cable.
#include <Arduino.h>

#include "BoardConfig.h"
#include "Esp32Adapters.h"
#include "Esp32Net.h"
#include "Esp32RxUart.h"
#include "Listener.h"
#include "RawCapture.h"

#ifndef LINKNET_BAUD
#define LINKNET_BAUD 76800
#endif
#ifndef LINKNET_STATS_PERIOD_S
#define LINKNET_STATS_PERIOD_S 60
#endif
#ifndef LINKNET_STALE_S
#define LINKNET_STALE_S 90
#endif
#ifndef LINKNET_LED_BRIGHTNESS
#define LINKNET_LED_BRIGHTNESS 24
#endif

using namespace linknet;

namespace {

ListenerConfig listenerConfig() {
    ListenerConfig c;
    c.baud = LINKNET_BAUD;
    c.statsPeriodS = LINKNET_STATS_PERIOD_S;
    c.staleS = LINKNET_STALE_S;
    return c;
}

esp32::RxOnlyUart  g_uart;
// What the decoder reads, also streamed whole on tcp/24 for analysis off the
// board (RawCapture.h).
RawCapture         g_raw(g_uart);
esp32::Esp32Clock  g_clock;
constexpr char kBuild[] = __DATE__ " " __TIME__;

esp32::ConsoleLog  g_log;
esp32::NetConsole  g_net;
esp32::NeoPixelLed g_led(LINKNET_LED_BRIGHTNESS);
Listener           g_listener(g_log, listenerConfig());
bool               g_listening = false;

char   g_input[96];
size_t g_inputLen = 0;

void printPins(const char *when) {
    char a[80], b[80];
    esp32::formatPin(a, sizeof a, board::kBusRxPin, esp32::readPin(board::kBusRxPin));
    esp32::formatPin(b, sizeof b, board::kReservedTxPin, esp32::readPin(board::kReservedTxPin));
    g_log.logf("PINS %s: %s | %s", when, a, b);
}

// The safety rules of docs/hardware.md, checked on the registers themselves: GPIO 17 not
// driven and not handed to U1TXD (IO MUX function 2), GPIO 18 without
// pull-down. If either fails, the UART is not started.
bool pinsSafe() {
    const esp32::PinState rx = esp32::readPin(board::kBusRxPin);
    const esp32::PinState tx = esp32::readPin(board::kReservedTxPin);
    return !rx.pullDown && !rx.output && !tx.output && tx.function != 2;
}

// A line typed on the console comes back as a marker, so a PuTTY log can
// carry the test steps too. UART0 only: this is the console, not the bus.
void readConsole(uint64_t nowUs) {
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (g_inputLen == 0) continue;
            g_input[g_inputLen] = '\0';
            g_log.logf("%7lu.%03lu MARK %s", static_cast<unsigned long>(nowUs / 1000000u),
                       static_cast<unsigned long>(nowUs / 1000u % 1000u), g_input);
            g_inputLen = 0;
        } else if (g_inputLen + 1 < sizeof g_input && c >= 0x20) {
            g_input[g_inputLen++] = static_cast<char>(c);
        }
    }
}

bool startListening() {
    if (!pinsSafe()) {
        g_log.write("REFUSING TO LISTEN: pin state breaks the safety rules above");
        return false;
    }
    const bool started = g_uart.begin(board::kBusRxPin, LINKNET_BAUD);
    printPins("listening");
    if (!started) {
        g_log.write("UART1 setup failed");
        return false;
    }
    if (!pinsSafe()) {
        g_log.write("REFUSING TO LISTEN: pin state changed by the UART setup");
        return false;
    }
    g_log.logf("statistics every %d s; lines: Zn ... (changed fields), key=, RETRY, ODD, ERR, BUS",
               LINKNET_STATS_PERIOD_S);
    return true;
}

}  // namespace

void setup() {
    // Room for a whole statistics block, so println() returns at once
    // instead of waiting for UART0 to send it.
    Serial.setTxBufferSize(4096);
    Serial.begin(board::kConsoleBaud);
    g_log.begin();
    g_led.begin();
    g_led.show(Rgb{0, 0, 255});
    delay(100);

    g_log.write("");
    g_log.logf("LINKnet listener, built %s", kBuild);
    g_log.logf("RECEIVE ONLY: UART1 RX=GPIO%d, %d baud 8N1, TX not routed, GPIO%d untouched",
               board::kBusRxPin, LINKNET_BAUD, board::kReservedTxPin);
    printPins("at boot");
    g_listening = startListening();
    // Even when refusing to listen: the refusal is then readable, and a
    // fixed image installable, without the cable.
    g_net.begin(g_log, "listener", kBuild, &g_raw);
}

void loop() {
    const uint64_t now = g_clock.nowUs();
    readConsole(now);
    if (!g_listening) {
        g_led.show(esp32::blinkUnlessOnline(Rgb{255, 0, 0}, g_net.online(), millis()));
        delay(50);
        return;
    }
    g_listener.poll(g_raw);
    g_listener.tick(g_clock.nowUs(), g_raw.health());
    g_led.show(esp32::blinkUnlessOnline(g_listener.status(g_clock.nowUs()), g_net.online(), millis()));
    delay(2);
}
