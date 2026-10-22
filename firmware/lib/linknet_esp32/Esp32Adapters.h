// Arduino adapters. Thin by design: the decoding lives in linknet_core,
// which has no Arduino dependency and is what the native tests exercise.
#pragma once

#include <Arduino.h>
#include <esp_timer.h>

#include "Ports.h"

namespace linknet {
namespace esp32 {

// The clock RxOnlyUart stamps bytes with: esp_timer, microseconds since boot.
class Esp32Clock : public IClock {
public:
    uint64_t nowUs() const override { return static_cast<uint64_t>(esp_timer_get_time()); }
};

// Console lines on UART0 (the CH343 port).
class SerialLog : public ILog {
public:
    void write(const char *line) override { Serial.println(line); }
};

// The on-board WS2812B on GPIO 38.
class NeoPixelLed : public ILed {
public:
    explicit NeoPixelLed(uint8_t brightness) : brightness_(brightness) {}
    // Parks the pixel dark. Must be the first call: neopixelWrite() latches
    // its pin on first use (seen on an earlier bench, arduino-esp32 2.0.17).
    void begin();
    void show(const Rgb &c) override;

private:
    uint8_t brightness_;
    Rgb     last_;
    bool    begun_ = false;
};

// What the pad registers say about a GPIO, read back rather than assumed,
// so the boot banner can show the safety rules hold on the real chip.
struct PinState {
    unsigned function = 0;    // IO MUX function (datasheet table 2-3)
    bool     input = false;   // FUN_IE
    bool     pullUp = false;  // FUN_WPU
    bool     pullDown = false;  // FUN_WPD
    bool     output = false;  // GPIO_ENABLE_REG: the pad is driven
    int      level = -1;
};
PinState readPin(int gpio);
// "gpio18 func=2 ie=1 pu=1 pd=0 oe=0 level=1"
void formatPin(char *buf, size_t cap, int gpio, const PinState &s);

}  // namespace esp32
}  // namespace linknet
