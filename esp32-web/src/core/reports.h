// JSON and Prometheus views of the GPSDO state, shared by the web server and the PC tests.
#pragma once

#include <string>

#include "gpsdo.h"

// Things only the web layer knows
struct HostInfo {
    uint32_t    uptime_s = 0;
    uint32_t    free_heap = 0;
    int         rssi = 0;
    std::string ip;
    std::string hostname;
    bool        ap_mode = false;
};

std::string statusJson(const Gpsdo& gpsdo, const HostInfo& host, uint32_t now);
std::string configJson(const Gpsdo& gpsdo);
std::string metricsText(const Gpsdo& gpsdo, const HostInfo& host, uint32_t now);
