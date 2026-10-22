// The board on the home Wi-Fi: its console over TCP, a status page, and
// firmware updates over HTTP.
//
// Why: during the tests the board sits by thermostat 1 with only a charger
// on its USB port, out of reach of the laptop's cable. The laptop then reads
// the console with `console.ps1 -Address <ip>` and installs the next image
// with `flash.ps1 -Address <ip>` (curl.exe, part of Windows: nothing
// installed on the host). The board also answers to linknet.local over
// mDNS, but on the bench laptop (two interfaces on the same LAN) Windows
// resolved that name only now and then, 2026-10-17: use the address the
// board logs on "WIFI connected ip=".
//
// None of this touches the bus. The network runs in its own task on core 0,
// where the Wi-Fi driver lives; the bus UART's interrupt is allocated from
// setup(), on core 1, at level 3 and in IRAM (Esp32RxUart.cpp). The main
// loop never waits on the network: it only appends lines to a queue.
//
//   tcp/23    the console, read-only: the same lines as UART0, queued so a
//             client that connects late still gets the boot banner
//   tcp/24    every byte of the bus with its time (RawCapture.h), binary,
//             for raw.ps1; only when begin() is given a RawCapture
//   http/80   GET /         status (image, uptime, Wi-Fi, console queue)
//             POST /update  a new image, multipart field "firmware", basic
//                           auth user "linknet" with the password from NVS
//
// An image installed over Wi-Fi boots "pending verify". It confirms itself
// once it has been online for 10 s; if it reboots first, or never gets
// online within 3 minutes, the bootloader goes back to the previous image.
// A broken update then costs a reboot, not a trip to the thermostat.
#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>

#include "LineQueue.h"
#include "NetSettings.h"
#include "RawCapture.h"
#include "Ports.h"

#ifndef LINKNET_HOSTNAME
#define LINKNET_HOSTNAME "linknet"
#endif
#ifndef LINKNET_NET_QUEUE
#define LINKNET_NET_QUEUE 16384  // ~6 min of listener output for a client that is away
#endif

namespace linknet {
namespace esp32 {

// The console: every line goes out on UART0 at once and into the queue for
// tcp/23. Called from the main loop and from the network task, hence the
// mutex (a FreeRTOS one: it never masks interrupts, so it cannot delay the
// bus UART's).
class ConsoleLog : public ILog {
public:
    ConsoleLog() : queue_(storage_, sizeof storage_) {}
    // After Serial.begin(), before a second task logs.
    void begin();
    void write(const char *line) override;

    // The network task's side of the queue.
    size_t   peek(char *dst, size_t cap);
    void     consume(size_t n);
    size_t   queued();
    uint32_t droppedLines();

private:
    SemaphoreHandle_t mutex_ = nullptr;
    StaticSemaphore_t mutexBuf_;
    char              storage_[LINKNET_NET_QUEUE];
    LineQueue         queue_;
};

class NetConsole {
public:
    // Logs which image is running, loads the settings from NVS and starts
    // the network task. `name` and `build` identify the image on the status
    // page. Returns false, and the board stays on USB only, when it was
    // never provisioned.
    bool begin(ConsoleLog &log, const char *name, const char *build, RawCapture *raw = nullptr);

    // Associated and holding an address.
    bool online() const { return online_.load(std::memory_order_relaxed); }

private:
    static void taskEntry(void *arg);
    void run();
    void startServers();
    void serveConsole();
    void serveRaw();
    void flushConsole(uint32_t ms);
    void checkImage(uint32_t now, uint32_t onlineSince);
    void report();
    void statusPage();
    void receiveUpdate();
    void finishUpdate();

    ConsoleLog  *log_ = nullptr;
    const char  *name_ = "";
    const char  *build_ = "";
    NetSettings  settings_;
    TaskHandle_t task_ = nullptr;

    std::atomic<bool> online_{false};
    bool     serversUp_ = false;
    bool     pendingVerify_ = false;
    uint32_t joins_ = 0;

    WiFiServer logServer_{23};
    WiFiClient client_;
    RawCapture *raw_ = nullptr;
    WiFiServer rawServer_{24};
    WiFiClient rawClient_;
    uint8_t    rawBuf_[1440];  // whole records; one TCP segment
    size_t     rawLen_ = 0;
    size_t     rawOff_ = 0;
    WebServer  http_{80};

    bool     updateAuthorized_ = false;
    bool     updateOk_ = false;
    char     updateError_[64] = {};
    uint32_t updateBytes_ = 0;
};

// The status LED's colour, dark half of each second while the board is not
// on the Wi-Fi: blinking means "no network", whatever the colour says.
inline Rgb blinkUnlessOnline(const Rgb &c, bool online, uint32_t ms) {
    return online || (ms / 500) % 2 == 0 ? c : Rgb{};
}

}  // namespace esp32
}  // namespace linknet
