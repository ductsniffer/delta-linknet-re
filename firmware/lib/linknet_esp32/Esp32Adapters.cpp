#include "Esp32Adapters.h"

#include <driver/gpio.h>
#include <soc/gpio_periph.h>
#include <soc/gpio_reg.h>
#include <soc/io_mux_reg.h>

#include <cstdio>

#include "BoardConfig.h"

namespace linknet {
namespace esp32 {

void NeoPixelLed::begin() {
    neopixelWrite(board::kRgbLedPin, 0, 0, 0);
    last_ = Rgb{};
    begun_ = true;
}

void NeoPixelLed::show(const Rgb &c) {
    if (!begun_) begin();
    // Polled from loop(), mostly with the same colour: write changes only.
    if (c == last_) return;
    last_ = c;
    const uint8_t r = static_cast<uint8_t>(c.r * brightness_ / 255);
    const uint8_t g = static_cast<uint8_t>(c.g * brightness_ / 255);
    const uint8_t b = static_cast<uint8_t>(c.b * brightness_ / 255);
    // neopixelWrite() sends G,R,B; this board's LED expects R,G,B (BoardConfig.h).
    neopixelWrite(board::kRgbLedPin, g, r, b);
}

PinState readPin(int gpio) {
    PinState s;
    const uint32_t mux = REG_READ(GPIO_PIN_MUX_REG[gpio]);
    s.function = (mux >> MCU_SEL_S) & MCU_SEL_V;
    s.input = (mux & FUN_IE) != 0;
    s.pullUp = (mux & FUN_PU) != 0;
    s.pullDown = (mux & FUN_PD) != 0;
    const uint32_t enable = gpio < 32 ? REG_READ(GPIO_ENABLE_REG) : REG_READ(GPIO_ENABLE1_REG);
    s.output = ((enable >> (gpio % 32)) & 1u) != 0;
    s.level = gpio_get_level(static_cast<gpio_num_t>(gpio));
    return s;
}

void formatPin(char *buf, size_t cap, int gpio, const PinState &s) {
    snprintf(buf, cap, "gpio%d func=%u ie=%d pu=%d pd=%d oe=%d level=%d", gpio, s.function,
             s.input, s.pullUp, s.pullDown, s.output, s.level);
}

}  // namespace esp32
}  // namespace linknet
