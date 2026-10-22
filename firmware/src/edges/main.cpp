// Wiring check (docs/hardware.md): is there a signal on GPIO 18, without any risk?
//
// GPIO 18 is a plain input with the internal pull-up and no pull-down. The
// PCNT peripheral counts its edges, rising and falling, in hardware; once a
// second the count is printed on the console. No UART is configured on the
// bus, and GPIO 17 is not touched.
//
// At ~1.3 kB/s on the bus (docs/report.md section 3), expect thousands of edges a
// second. Zero edges with the level stuck at 1 means no signal reaches the
// pin: check the wiring, do not change pins.
//
// The console also goes out on tcp/23 once the board is on the Wi-Fi
// (Esp32Net.h), and the listener is installed over the Wi-Fi from here.
#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/pcnt.h>

#include "BoardConfig.h"
#include "Esp32Adapters.h"
#include "Esp32Net.h"

using namespace linknet;

namespace {

constexpr pcnt_unit_t kUnit = PCNT_UNIT_0;

// The counter is 16 bits. At 76 800 baud a line can toggle at most 76 800
// times a second, so reading every 100 ms keeps it under 7 680.
constexpr uint32_t kSampleMs = 100;

constexpr char kBuild[] = __DATE__ " " __TIME__;

esp32::ConsoleLog  g_log;
esp32::NetConsole  g_net;
esp32::NeoPixelLed g_led(24);
Rgb                g_colour{0, 0, 255};
bool               g_ok = false;

uint32_t g_lastSampleMs = 0;
uint32_t g_lastPrintMs = 0;
uint32_t g_second = 0;     // edges in the current second
uint32_t g_peak = 0;       // most edges in one 100 ms sample this second
uint64_t g_total = 0;
uint32_t g_silentSeconds = 0;

void printPins(const char *when) {
    char a[80], b[80];
    esp32::formatPin(a, sizeof a, board::kBusRxPin, esp32::readPin(board::kBusRxPin));
    esp32::formatPin(b, sizeof b, board::kReservedTxPin, esp32::readPin(board::kReservedTxPin));
    g_log.logf("PINS %s: %s | %s", when, a, b);
}

bool setUpCounter() {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << board::kBusRxPin;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&io) != ESP_OK) return false;

    pcnt_config_t c = {};
    c.pulse_gpio_num = board::kBusRxPin;
    c.ctrl_gpio_num = PCNT_PIN_NOT_USED;
    c.lctrl_mode = PCNT_MODE_KEEP;
    c.hctrl_mode = PCNT_MODE_KEEP;
    c.pos_mode = PCNT_COUNT_INC;  // rising edges
    c.neg_mode = PCNT_COUNT_INC;  // and falling edges
    c.counter_h_lim = 32767;
    c.counter_l_lim = -1;
    c.unit = kUnit;
    c.channel = PCNT_CHANNEL_0;
    if (pcnt_unit_config(&c) != ESP_OK) return false;
    // pcnt_unit_config() sets the pin pull-up itself; restate both pulls.
    gpio_pulldown_dis(static_cast<gpio_num_t>(board::kBusRxPin));
    gpio_pullup_en(static_cast<gpio_num_t>(board::kBusRxPin));

    // Ignore pulses shorter than 100 APB cycles (1.25 us): one bit at
    // 76 800 baud lasts 13 us, so nothing real is filtered out.
    pcnt_set_filter_value(kUnit, 100);
    pcnt_filter_enable(kUnit);
    pcnt_counter_pause(kUnit);
    pcnt_counter_clear(kUnit);
    pcnt_counter_resume(kUnit);
    return true;
}

}  // namespace

void setup() {
    Serial.setTxBufferSize(4096);
    Serial.begin(board::kConsoleBaud);
    g_log.begin();
    g_led.begin();
    g_led.show(g_colour);
    delay(100);
    g_log.write("");
    g_log.logf("LINKnet edge counter (step 3), built %s", kBuild);
    g_log.logf("GPIO%d input with pull-up, edges counted by PCNT; no UART on the bus; GPIO%d untouched",
               board::kBusRxPin, board::kReservedTxPin);
    printPins("at boot");
    g_ok = setUpCounter();
    printPins("counting");
    if (!g_ok) {
        g_log.write("PCNT setup failed");
        g_colour = Rgb{255, 0, 0};
    }
    g_net.begin(g_log, "edges", kBuild);
    g_lastSampleMs = g_lastPrintMs = millis();
}

void loop() {
    const uint32_t now = millis();
    g_led.show(esp32::blinkUnlessOnline(g_colour, g_net.online(), now));
    if (!g_ok) {
        delay(100);
        return;
    }
    if (now - g_lastSampleMs >= kSampleMs) {
        g_lastSampleMs = now;
        int16_t count = 0;
        pcnt_get_counter_value(kUnit, &count);
        pcnt_counter_clear(kUnit);
        const uint32_t n = count > 0 ? static_cast<uint32_t>(count) : 0;
        g_second += n;
        if (n > g_peak) g_peak = n;
    }
    if (now - g_lastPrintMs >= 1000) {
        g_lastPrintMs = now;
        g_total += g_second;
        g_silentSeconds = g_second == 0 ? g_silentSeconds + 1 : 0;
        g_log.logf("%7lu s EDGES %5lu/s peak %4lu/100ms level=%d total=%llu%s",
                   static_cast<unsigned long>(now / 1000), static_cast<unsigned long>(g_second),
                   static_cast<unsigned long>(g_peak), gpio_get_level(static_cast<gpio_num_t>(board::kBusRxPin)),
                   static_cast<unsigned long long>(g_total), g_silentSeconds ? "  (no signal)" : "");
        // Green while edges come in, red after a silent second.
        g_colour = g_second ? Rgb{0, 255, 0} : Rgb{255, 0, 0};
        g_second = 0;
        g_peak = 0;
    }
    delay(1);
}
