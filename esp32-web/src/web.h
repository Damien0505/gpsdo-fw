// HTTP API, WebSocket, setup and firmware update pages.
#pragma once

#include <Arduino.h>

#include "core/gpsdo.h"
#include "device_settings.h"

namespace web {

// Whatever touches gpsdo must hold the lock: the web server runs in its own task
void lock();
void unlock();

void begin(Gpsdo& gpsdo, DeviceSettings& settings);
// Pushes to WebSocket clients, handles delayed reboot
void loop(uint32_t now);
void updateMutexInit();

}  // namespace web
