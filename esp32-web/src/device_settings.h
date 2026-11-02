// Network settings kept in NVS, entered through the setup page.
#pragma once

#include <Arduino.h>

struct DeviceSettings {
    String ssid;
    String password;
    String hostname = "gpsdo";
    String admin;
    bool   static_ip = false;
    String ip;
    String gateway;
    String subnet;
    String dns;

    bool provisioned() const { return ssid.length() > 0 && admin.length() > 0; }
    void load();
    void save() const;
    static void erase();
};
