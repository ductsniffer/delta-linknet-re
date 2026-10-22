#include "Esp32Net.h"

#include <ESPmDNS.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>

#include <cstdio>
#include <cstring>

#include "Esp32Settings.h"

// Arduino-ESP32 2.0.17 is built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE,
// and initArduino() confirms every image at boot unless this returns true
// (cores/esp32/esp32-hal-misc.c). Returning true hands the decision to
// NetConsole::checkImage(): an image is good once it has been online.
extern "C" bool verifyRollbackLater() { return true; }

namespace linknet {
namespace esp32 {

namespace {

constexpr uint16_t kLogPort = 23;
constexpr uint16_t kRawPort = 24;
constexpr uint16_t kHttpPort = 80;
constexpr char     kOtaUser[] = "linknet";

// The 2.0.17 event handler rejoins after a first failure and after
// "reconnectable" reasons only (libraries/WiFi/src/WiFiGeneric.cpp): an
// access point that is absent at boot, or a handshake timeout, leaves the
// station idle for good. So after this long without an address, start over.
constexpr uint32_t kRejoinMs = 30000;
constexpr uint32_t kConfirmMs = 10000;
constexpr uint32_t kGiveUpMs = 180000;
constexpr uint32_t kReportMs = 60000;

const char *imageState(esp_ota_img_states_t s) {
    switch (s) {
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_PENDING_VERIFY: return "installed over Wi-Fi, not yet confirmed";
        case ESP_OTA_IMG_VALID: return "confirmed";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        default: return "flashed over USB";
    }
}

}  // namespace

// ------------------------------------------------------------------ ConsoleLog

void ConsoleLog::begin() { mutex_ = xSemaphoreCreateMutexStatic(&mutexBuf_); }

void ConsoleLog::write(const char *line) {
    if (mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
    Serial.println(line);
    queue_.push(line);
    if (mutex_) xSemaphoreGive(mutex_);
}

size_t ConsoleLog::peek(char *dst, size_t cap) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const size_t n = queue_.peek(dst, cap);
    xSemaphoreGive(mutex_);
    return n;
}

void ConsoleLog::consume(size_t n) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    queue_.consume(n);
    xSemaphoreGive(mutex_);
}

size_t ConsoleLog::queued() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const size_t n = queue_.size();
    xSemaphoreGive(mutex_);
    return n;
}

uint32_t ConsoleLog::droppedLines() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const uint32_t n = queue_.droppedLines();
    xSemaphoreGive(mutex_);
    return n;
}

// ------------------------------------------------------------------ NetConsole

bool NetConsole::begin(ConsoleLog &log, const char *name, const char *build, RawCapture *raw) {
    log_ = &log;
    raw_ = raw;
    name_ = name;
    build_ = build;

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(running, &state);
    pendingVerify_ = state == ESP_OTA_IMG_PENDING_VERIFY;
    log.logf("BOOT image %s in %s (%s)", name, running ? running->label : "?", imageState(state));

    if (!loadNetSettings(settings_) || !settings_.complete()) {
        log.write("WIFI not provisioned (env:provision, provision.ps1): console on USB only");
        return false;
    }
    log.logf("WIFI joining; %s.local: console tcp/%u, status and updates http/%u", LINKNET_HOSTNAME,
             kLogPort, kHttpPort);
    // Core 0 with the Wi-Fi driver; priority 1, below everything that matters.
    xTaskCreatePinnedToCore(&NetConsole::taskEntry, "net", 8192, this, 1, &task_, 0);
    return true;
}

void NetConsole::taskEntry(void *arg) { static_cast<NetConsole *>(arg)->run(); }

void NetConsole::run() {
    // The settings live in our own NVS keys; the driver's copy would be a
    // second, stale one.
    WiFi.persistent(false);
    WiFi.setHostname(LINKNET_HOSTNAME);
    WiFi.mode(WIFI_STA);
    // Modem sleep holds incoming packets until the next beacon it wakes for;
    // the board runs on a charger, so stay awake and answer at once.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(settings_.ssid, settings_.pass);

    uint32_t lastJoin = millis();
    uint32_t lastReport = millis();
    uint32_t onlineSince = 0;
    for (;;) {
        const uint32_t now = millis();
        const bool up = WiFi.status() == WL_CONNECTED;
        if (up && !online()) {
            online_.store(true, std::memory_order_relaxed);
            onlineSince = now;
            ++joins_;
            log_->logf("WIFI connected ip=%s rssi=%d dBm channel=%d", WiFi.localIP().toString().c_str(),
                       WiFi.RSSI(), WiFi.channel());
            if (!serversUp_) startServers();
        } else if (!up && online()) {
            online_.store(false, std::memory_order_relaxed);
            lastJoin = now;
            if (client_) client_.stop();
            if (rawClient_) rawClient_.stop();
            log_->write("WIFI lost");
        }
        if (!up && now - lastJoin >= kRejoinMs) {
            lastJoin = now;
            WiFi.disconnect();
            WiFi.begin(settings_.ssid, settings_.pass);
        }
        if (up) {
            serveConsole();
            serveRaw();
            http_.handleClient();
        }
        checkImage(now, onlineSince);
        if (now - lastReport >= kReportMs) {
            lastReport = now;
            report();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void NetConsole::startServers() {
    serversUp_ = true;
    logServer_.begin(kLogPort);
    logServer_.setNoDelay(true);
    if (raw_) rawServer_.begin(kRawPort);
    http_.on("/", HTTP_GET, [this]() { statusPage(); });
    http_.on("/update", HTTP_POST, [this]() { finishUpdate(); }, [this]() { receiveUpdate(); });
    http_.onNotFound([this]() { http_.send(404, "text/plain", "not found\n"); });
    http_.begin(kHttpPort);
    if (MDNS.begin(LINKNET_HOSTNAME)) {
        MDNS.addService("http", "tcp", kHttpPort);
    } else {
        log_->write("NET mDNS failed: use the address above instead of " LINKNET_HOSTNAME ".local");
    }
}

void NetConsole::serveConsole() {
    if (logServer_.hasClient()) {
        // One client. A new one replaces the old, which may be a connection
        // the laptop gave up on without the board noticing.
        if (client_) client_.stop();
        client_ = logServer_.available();
        client_.setNoDelay(true);
        char hello[160];
        snprintf(hello, sizeof hello, "NET hello: LINKnet %s, built %s, up %lu s; %u bytes queued before this\n",
                 name_, build_, static_cast<unsigned long>(millis() / 1000),
                 static_cast<unsigned>(log_->queued()));
        client_.write(hello, strlen(hello));
        log_->logf("NET console client %s connected", client_.remoteIP().toString().c_str());
    }
    if (!client_) return;
    if (!client_.connected()) {
        client_.stop();
        log_->write("NET console client gone");
        return;
    }
    // Read-only console: whatever the client sends is discarded.
    while (client_.available() > 0) client_.read();
    char buf[1024];
    const size_t n = log_->peek(buf, sizeof buf);
    if (n == 0) return;
    // May block for seconds on a stalled connection: this task only.
    const size_t sent = client_.write(reinterpret_cast<const uint8_t *>(buf), n);
    log_->consume(sent);
    if (sent == 0) {
        client_.stop();
        log_->write("NET console client stalled, closed");
    }
}

// Records go out whole: a buffer is refilled only once fully sent, so a
// client always starts on a record boundary, after a one-line text header
// that ties the board's clock to the moment it connected.
void NetConsole::serveRaw() {
    if (!raw_) return;
    if (rawServer_.hasClient()) {
        if (rawClient_) rawClient_.stop();
        rawClient_ = rawServer_.available();
        rawClient_.setNoDelay(true);
        char hello[192];
        snprintf(hello, sizeof hello, "LNKRAW1 image=%s build=%s now_us=%llu record=6 pending=%lu lost=%lu\n",
                 name_, build_, static_cast<unsigned long long>(esp_timer_get_time()),
                 static_cast<unsigned long>(raw_->pending()), static_cast<unsigned long>(raw_->lostBytes()));
        rawClient_.write(hello, strlen(hello));
        rawLen_ = rawOff_ = 0;
        log_->logf("NET raw client %s connected", rawClient_.remoteIP().toString().c_str());
    }
    if (!rawClient_) return;
    if (!rawClient_.connected()) {
        rawClient_.stop();
        log_->write("NET raw client gone");
        return;
    }
    while (rawClient_.available() > 0) rawClient_.read();
    // A few buffers per pass at most: the console and HTTP get their turn.
    for (int i = 0; i < 4; ++i) {
        if (rawOff_ == rawLen_) {
            rawOff_ = 0;
            rawLen_ = raw_->drain(rawBuf_, sizeof rawBuf_);
            if (rawLen_ == 0) return;
        }
        const size_t sent = rawClient_.write(rawBuf_ + rawOff_, rawLen_ - rawOff_);
        if (sent == 0) {
            rawClient_.stop();
            log_->write("NET raw client stalled, closed");
            return;
        }
        rawOff_ += sent;
    }
}

void NetConsole::flushConsole(uint32_t ms) {
    const uint32_t start = millis();
    while (client_ && log_->queued() > 0 && millis() - start < ms) serveConsole();
}

void NetConsole::checkImage(uint32_t now, uint32_t onlineSince) {
    if (!pendingVerify_) return;
    if (online() && now - onlineSince >= kConfirmMs) {
        pendingVerify_ = false;
        esp_ota_mark_app_valid_cancel_rollback();
        log_->write("OTA this image got online: confirmed, no rollback");
    } else if (!online() && now >= kGiveUpMs) {
        log_->write("OTA this image never got online: back to the previous image");
        Serial.flush();
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

void NetConsole::report() {
    if (online()) {
        log_->logf("NET rssi=%d dBm joins=%lu client=%s queued=%u dropped=%lu", WiFi.RSSI(),
                   static_cast<unsigned long>(joins_),
                   client_ ? client_.remoteIP().toString().c_str() : "none",
                   static_cast<unsigned>(log_->queued()), static_cast<unsigned long>(log_->droppedLines()));
        if (raw_) {
            log_->logf("NET raw client=%s pending=%lu lost=%lu", rawClient_ ? "yes" : "none",
                       static_cast<unsigned long>(raw_->pending()), static_cast<unsigned long>(raw_->lostBytes()));
        }
    } else {
        log_->logf("NET offline, Wi-Fi status %d, joins=%lu", static_cast<int>(WiFi.status()),
                   static_cast<unsigned long>(joins_));
    }
}

void NetConsole::statusPage() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(running, &state);
    char page[512];
    snprintf(page, sizeof page,
             "LINKnet %s, built %s\n"
             "uptime %lu s, image in %s (%s)\n"
             "wifi ip %s, rssi %d dBm, channel %d, joins %lu\n"
             "console tcp/%u: client %s, %u bytes queued, %lu lines dropped\n"
             "update: POST /update, multipart field \"firmware\", basic auth user \"%s\"\n",
             name_, build_, static_cast<unsigned long>(millis() / 1000), running ? running->label : "?",
             imageState(state), WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel(),
             static_cast<unsigned long>(joins_), kLogPort,
             client_ ? client_.remoteIP().toString().c_str() : "none", static_cast<unsigned>(log_->queued()),
             static_cast<unsigned long>(log_->droppedLines()), kOtaUser);
    http_.send(200, "text/plain", page);
}

// Called by WebServer for each piece of the multipart body. Modelled on
// libraries/HTTPUpdateServer (2.0.17), with our own log lines: the password
// is checked once, on the first piece, before anything is erased.
void NetConsole::receiveUpdate() {
    HTTPUpload &up = http_.upload();
    switch (up.status) {
        case UPLOAD_FILE_START:
            updateOk_ = false;
            updateError_[0] = '\0';
            updateBytes_ = 0;
            updateAuthorized_ = http_.authenticate(kOtaUser, settings_.otaPass);
            if (!updateAuthorized_) {
                log_->logf("OTA refused from %s: wrong password", http_.client().remoteIP().toString().c_str());
                return;
            }
            log_->logf("OTA receiving %s from %s", up.filename.c_str(), http_.client().remoteIP().toString().c_str());
            // Update picks the other app slot and erases it as it goes: the
            // running image is untouched whatever happens next.
            if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                snprintf(updateError_, sizeof updateError_, "begin: %s", Update.errorString());
            }
            break;
        case UPLOAD_FILE_WRITE:
            if (!updateAuthorized_ || updateError_[0]) return;
            if (Update.write(up.buf, up.currentSize) != up.currentSize) {
                snprintf(updateError_, sizeof updateError_, "write: %s", Update.errorString());
            }
            updateBytes_ += up.currentSize;
            break;
        case UPLOAD_FILE_END:
            if (!updateAuthorized_ || updateError_[0]) return;
            // end() checks the image (header, checksum, SHA-256) before it
            // makes it the one to boot.
            if (Update.end(true)) {
                updateOk_ = true;
            } else {
                snprintf(updateError_, sizeof updateError_, "end: %s", Update.errorString());
            }
            break;
        case UPLOAD_FILE_ABORTED:
            if (!updateAuthorized_) return;
            Update.abort();
            snprintf(updateError_, sizeof updateError_, "upload aborted");
            break;
    }
}

void NetConsole::finishUpdate() {
    const bool authorized = updateAuthorized_;
    updateAuthorized_ = false;
    if (!authorized) {
        http_.requestAuthentication();
        return;
    }
    if (!updateOk_) {
        if (!updateError_[0]) snprintf(updateError_, sizeof updateError_, "no \"firmware\" part");
        log_->logf("OTA failed: %s; still running the same image", updateError_);
        char msg[96];
        snprintf(msg, sizeof msg, "update failed: %s\n", updateError_);
        http_.send(500, "text/plain", msg);
        return;
    }
    log_->logf("OTA %lu bytes written and verified; restarting into the new image",
               static_cast<unsigned long>(updateBytes_));
    http_.send(200, "text/plain", "OK, restarting\n");
    flushConsole(1000);
    if (rawClient_) rawClient_.stop();
    // Closed, not just abandoned: console.ps1 then sees the end at once and
    // reconnects to the new image, instead of waiting out its 150 s timeout
    // (bench, 2026-10-17).
    client_.stop();
    Serial.flush();
    delay(200);
    ESP.restart();
}

}  // namespace esp32
}  // namespace linknet
