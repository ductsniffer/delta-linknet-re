// Hardware wiring of the listener: a Waveshare ESP32-S3-DEV-KIT-N8R8 and a
// Waveshare TTL TO RS485 (C) module, wired as in docs/hardware.md.
//
// THE BUS IS RECEIVE-ONLY, TWICE OVER:
//   - in hardware, the module's RXD (its input, what it would transmit) is
//     tied to its own VCC and reaches no ESP32 pin;
//   - in software, UART1 is given a receive pin and nothing else, and no
//     code writes to it (lib/linknet_esp32/Esp32RxUart.cpp).
#pragma once

#include <cstdint>

namespace board {

// --- Bus receive: module TXD -> GPIO 18 ------------------------------------
// The module names its pins from its own side: its TXD is what it SENDS to
// the microcontroller, i.e. what it hears on the bus. Waveshare's own
// point-to-point drawing (https://www.waveshare.com/wiki/TTL_TO_RS485_(C))
// wires the MCU's RXD to the module's TXD.
//
// 18 because it is U1RXD in the IO MUX (Espressif ESP32-S3
// datasheet, table 2-3: GPIO18 function 2 = U1RXD), so
// UART1 takes it without the GPIO matrix, and because it is none of the pins
// this board reserves (below). Input with the internal pull-up, NEVER a
// pull-down: with the module unpowered or unplugged the line then idles high,
// which a UART reads as silence rather than as a stream of zero bytes.
constexpr int kBusRxPin = 18;

// --- GPIO 17: deliberately unused -----------------------------------------
// U1TXD in the IO MUX (same table). Kept free for a possible transmitting
// phase, which needs explicit approval. Nothing configures it: after reset
// it is an input with no pull and no output driver (datasheet table 2-1,
// "IE" after reset), and the firmware prints its output-enable bit at boot
// to show it stayed that way.
constexpr int kReservedTxPin = 17;

// --- On-board RGB LED ---------------------------------------------------------
// Board schematic (https://www.waveshare.com/wiki/ESP32-S3-DEV-KIT-N8R8): GPIO38 ->
// R5 (0R) -> DIN of the WS2812B. Measured on an earlier project with this
// same board: the part takes R,G,B on the wire, not the G,R,B of a genuine
// WS2812B (sent red, it lit green). Esp32Adapters.cpp swaps accordingly.
constexpr int kRgbLedPin = 38;

// --- Pins deliberately avoided (docs/report.md section 10) ------------------
//   0, 3, 45, 46   strapping pins
//   19, 20         native USB D-/D+
//   35, 36, 37     octal PSRAM on this N8R8 module: the WROOM-1 datasheet
//                  reserves them for the PSRAM on octal-PSRAM variants
//   38             RGB LED, above
//   43, 44         UART0: the CH343P bridge, boot log and console

// --- Console ----------------------------------------------------------------
// UART0 through the CH343P: the COM port with automatic reset, the one
// esptool flashes through and PuTTY opens. Never the bus.
constexpr uint32_t kConsoleBaud = 115200;

}  // namespace board
