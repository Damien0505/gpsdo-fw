// WiFi station, setup access point with captive portal, mDNS.
#pragma once

#include <Arduino.h>

#include "device_settings.h"

namespace network {

void begin(const DeviceSettings& settings);
void loop(uint32_t now);

bool connected();
bool accessPointActive();
String ipAddress();

}  // namespace network
