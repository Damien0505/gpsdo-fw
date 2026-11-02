#include "device_settings.h"

#include <Preferences.h>

static const char NAMESPACE[] = "gpsdo";

void DeviceSettings::load()
{
    Preferences prefs;
    prefs.begin(NAMESPACE, true);
    ssid = prefs.getString("ssid", "");
    password = prefs.getString("password", "");
    hostname = prefs.getString("hostname", "gpsdo");
    admin = prefs.getString("admin", "");
    static_ip = prefs.getBool("static", false);
    ip = prefs.getString("ip", "");
    gateway = prefs.getString("gateway", "");
    subnet = prefs.getString("subnet", "");
    dns = prefs.getString("dns", "");
    prefs.end();
}

void DeviceSettings::save() const
{
    Preferences prefs;
    prefs.begin(NAMESPACE, false);
    prefs.putString("ssid", ssid);
    prefs.putString("password", password);
    prefs.putString("hostname", hostname);
    prefs.putString("admin", admin);
    prefs.putBool("static", static_ip);
    prefs.putString("ip", ip);
    prefs.putString("gateway", gateway);
    prefs.putString("subnet", subnet);
    prefs.putString("dns", dns);
    prefs.end();
}

void DeviceSettings::erase()
{
    Preferences prefs;
    prefs.begin(NAMESPACE, false);
    prefs.clear();
    prefs.end();
}
