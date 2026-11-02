// ESP32-C3 Super Mini between the GPSDO's external serial link and the LAN. See README.md.
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_system.h>

#include "core/gpsdo.h"
#include "device_settings.h"
#include "network.h"
#include "web.h"

namespace {

constexpr int LINK_RX_PIN = 20;
constexpr int LINK_TX_PIN = 21;
constexpr uint32_t LINK_BAUD = 115200;
constexpr size_t LINK_RX_BUFFER = 2048;
constexpr int LED_PIN = 8;            // Active low
constexpr int BOOT_BUTTON_PIN = 9;    // Read only after boot, when it is safe
constexpr uint32_t FACTORY_RESET_MS = 5000;
constexpr uint32_t HEAP_LOG_PERIOD_MS = 3600000;

DeviceSettings settings;

Gpsdo gpsdo([](const char* data, size_t len) { Serial1.write(reinterpret_cast<const uint8_t*>(data), len); });

uint32_t last_heap_log = 0;
uint32_t button_since = 0;
bool button_down = false;

void readLink(uint32_t now)
{
    uint8_t buffer[256];
    size_t available;
    while ((available = Serial1.available()) > 0) {
        size_t count = Serial1.readBytes(buffer, min(available, sizeof(buffer)));
        web::lock();
        gpsdo.onBytes(buffer, count, now);
        web::unlock();
    }
}

// Solid: all good. Slow blink: no link with the GPSDO. Fast blink: no WiFi.
void updateLed(uint32_t now, bool link_up)
{
    bool on;
    if (!network::connected()) {
        on = (now / 125) % 2 == 0;
    } else if (!link_up) {
        on = (now / 500) % 2 == 0;
    } else {
        on = true;
    }
    digitalWrite(LED_PIN, on ? LOW : HIGH);
}

// In case the admin password is lost: back to the setup network
void checkFactoryReset(uint32_t now)
{
    bool down = digitalRead(BOOT_BUTTON_PIN) == LOW;
    if (down && !button_down) {
        button_since = now;
    }
    button_down = down;
    if (down && (now - button_since) >= FACTORY_RESET_MS) {
        Serial.println("Erasing settings");
        DeviceSettings::erase();
        ESP.restart();
    }
}

}  // namespace

void setup()
{
    Serial.begin(115200);
    // Never wait for a USB host that isn't there
    Serial.setTxTimeoutMs(0);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    Serial1.setRxBufferSize(LINK_RX_BUFFER);
    Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);

    web::updateMutexInit();
    settings.load();
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS not available");
    }
    network::begin(settings);
    web::begin(gpsdo, settings);
    Serial.printf("Started, free heap %u\n", (unsigned)ESP.getFreeHeap());
}

void loop()
{
    uint32_t now = millis();
    readLink(now);

    web::lock();
    gpsdo.tick(now);
    bool link_up = gpsdo.linkUp(now);
    web::unlock();

    network::loop(now);
    web::loop(now);
    updateLed(now, link_up);
    checkFactoryReset(now);

    if ((now - last_heap_log) >= HEAP_LOG_PERIOD_MS) {
        last_heap_log = now;
        Serial.printf("Uptime %lu s, free heap %u, largest block %u\n", (unsigned long)(now / 1000),
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    }
    delay(1);
}
