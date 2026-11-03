#include "network.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

namespace {

constexpr uint32_t WIFI_FAIL_MS = 60000;
const char AP_SSID[] = "GPSDO-Setup";
constexpr uint8_t DNS_PORT = 53;

DeviceSettings config;
DNSServer dns;
bool ap_active = false;
bool was_connected = false;
uint32_t last_connected = 0;

// Only takes effect once WiFi has started, so this comes after softAP() or begin()
void applyTxPower()
{
#ifdef WIFI_TX_POWER
    WiFi.setTxPower(WIFI_TX_POWER);
#endif
    // getTxPower() is in units of 0.25 dBm
    Serial.printf("WiFi transmit power setting %d (0.25 dBm units)\n", (int)WiFi.getTxPower());
}

void startAccessPoint()
{
    if (ap_active) {
        return;
    }
    // Keeps trying the configured network, if any, at the same time
    WiFi.mode(config.provisioned() ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAP(AP_SSID);
    applyTxPower();
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(DNS_PORT, "*", WiFi.softAPIP());
    ap_active = true;
    Serial.printf("Setup access point %s at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void stopAccessPoint()
{
    dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    ap_active = false;
    Serial.println("Setup access point stopped");
}

void startStation()
{
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(config.hostname.c_str());
    if (config.static_ip) {
        IPAddress ip, gateway, subnet, dns_server;
        if (ip.fromString(config.ip) && gateway.fromString(config.gateway) && subnet.fromString(config.subnet)) {
            if (!dns_server.fromString(config.dns)) {
                dns_server = gateway;
            }
            WiFi.config(ip, gateway, subnet, dns_server);
        } else {
            Serial.println("Static IP settings not valid, using DHCP");
        }
    }
    WiFi.setAutoReconnect(true);
    // Answers quicker, and mDNS is more reliable without modem sleep. The board is on USB power.
    WiFi.setSleep(false);
    WiFi.begin(config.ssid.c_str(), config.password.c_str());
    applyTxPower();
    Serial.printf("Connecting to %s\n", config.ssid.c_str());
}

void onConnected()
{
    Serial.printf("Connected: http://%s/ and http://%s.local/\n", WiFi.localIP().toString().c_str(), config.hostname.c_str());
    MDNS.end();
    if (MDNS.begin(config.hostname.c_str())) {
        MDNS.addService("http", "tcp", 80);
    } else {
        Serial.println("mDNS failed");
    }
}

}  // namespace

namespace network {

void begin(const DeviceSettings& settings)
{
    config = settings;
    WiFi.persistent(false);
    last_connected = millis();
    if (config.provisioned()) {
        startStation();
    } else {
        startAccessPoint();
    }
}

void loop(uint32_t now)
{
    if (ap_active) {
        dns.processNextRequest();
    }
    if (!config.provisioned()) {
        return;
    }
    bool is_connected = WiFi.status() == WL_CONNECTED;
    if (is_connected) {
        last_connected = now;
        if (!was_connected) {
            onConnected();
        }
        if (ap_active) {
            stopAccessPoint();
        }
    } else if (!ap_active && (now - last_connected) >= WIFI_FAIL_MS) {
        Serial.println("No WiFi for 60 s");
        startAccessPoint();
    }
    was_connected = is_connected;
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

bool accessPointActive() { return ap_active; }

String ipAddress()
{
    return connected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
}

}  // namespace network
