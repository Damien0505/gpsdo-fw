#include "reports.h"

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

// Whole JSON numbers where possible, rounding what the GPS gives
static double rounded(double value, double factor)
{
    return std::round(value * factor) / factor;
}

std::string statusJson(const Gpsdo& gpsdo, const HostInfo& host, uint32_t now)
{
    JsonDocument doc;
    doc["type"] = "status";

    JsonObject link = doc["link"].to<JsonObject>();
    link["up"] = gpsdo.linkUp(now);
    uint32_t age = gpsdo.linkAgeMs(now);
    if (age == UINT32_MAX) {
        link["age_ms"] = nullptr;
    } else {
        link["age_ms"] = age;
    }
    link["bad_checksums"] = gpsdo.badChecksums();
    link["lines"] = gpsdo.linesReceived();
    link["down_events"] = gpsdo.linkDownEvents();

    const Telemetry& t = gpsdo.telemetry();
    if (t.valid) {
        JsonObject tlm = doc["tlm"].to<JsonObject>();
        tlm["uptime"] = t.uptime;
        tlm["gps_lock"] = t.gps_lock;
        tlm["ppb_lock"] = t.ppb_lock;
        tlm["sats"] = t.sats;
        if (t.ppb_mean == PPB_UNKNOWN) {
            tlm["ppb_mean"] = nullptr;
        } else {
            tlm["ppb_mean"] = t.ppb_mean;
        }
        tlm["ppb_inst"] = t.ppb_inst;
        tlm["freq"] = t.freq;
        tlm["error"] = t.error;
        tlm["correction"] = t.correction;
        tlm["pwm"] = t.pwm;
        tlm["millis"] = t.millis;
        tlm["pps_shift"] = t.pps_shift;
        tlm["pps_shift_ms"] = t.pps_shift_ms;
        tlm["pps_sync_count"] = t.pps_sync_count;
        tlm["adjusting"] = t.adjusting;
    } else {
        doc["tlm"] = nullptr;
    }

    const GpsData& g = gpsdo.gps();
    JsonObject gps = doc["gps"].to<JsonObject>();
    gps["time"] = g.has_time ? g.time : nullptr;
    gps["date"] = g.has_date ? g.date : nullptr;
    gps["fix_quality"] = g.fix_quality;
    gps["fix_mode"] = g.fix_mode;
    if (g.has_position) {
        gps["lat"] = rounded(g.latitude, 1e6);
        gps["lon"] = rounded(g.longitude, 1e6);
        gps["locator"] = g.locator;
    }
    if (g.has_altitude) {
        gps["alt"] = rounded(g.altitude, 10);
        gps["geoid"] = rounded(g.geoid, 10);
    }
    if (g.has_hdop) {
        gps["hdop"] = rounded(g.hdop, 100);
    }
    gps["sats_used"] = g.sats_used;
    gps["sats_view"] = g.sat_count;
    JsonArray sats = gps["sats"].to<JsonArray>();
    for (int i = 0; i < g.sat_count; i++) {
        const Satellite& s = g.sats[i];
        JsonArray row = sats.add<JsonArray>();
        row.add(s.talker);
        row.add(s.prn);
        row.add(s.elevation);
        row.add(s.azimuth);
        row.add(s.snr);
        row.add(s.used);
    }

    JsonObject esp = doc["esp"].to<JsonObject>();
    esp["uptime"] = host.uptime_s;
    esp["heap"] = host.free_heap;
    esp["rssi"] = host.rssi;
    esp["ip"] = host.ip;
    esp["hostname"] = host.hostname;
    esp["ap_mode"] = host.ap_mode;

    std::string out;
    serializeJson(doc, out);
    return out;
}

std::string configJson(const Gpsdo& gpsdo)
{
    JsonDocument doc;
    doc["type"] = "config";
    doc["complete"] = gpsdo.configComplete();
    doc["fw_version"] = gpsdo.firmwareVersion();
    doc["protocol"] = gpsdo.protocolVersion();
    JsonObject config = doc["config"].to<JsonObject>();
    const ConfigMap& map = gpsdo.config();
    for (int i = 0; i < map.size(); i++) {
        const ConfigMap::Entry& entry = map.at(i);
        if (entry.key == "fw_version") {
            continue;
        }
        char* end;
        long long number = std::strtoll(entry.value.c_str(), &end, 10);
        if (*end == '\0' && !entry.value.empty()) {
            config[entry.key] = number;
        } else {
            config[entry.key] = entry.value;
        }
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

static void metric(std::string& out, const char* name, const char* help, double value)
{
    char line[192];
    std::snprintf(line, sizeof(line), "# HELP %s %s\n# TYPE %s gauge\n%s %.10g\n", name, help, name, name, value);
    out += line;
}

std::string metricsText(const Gpsdo& gpsdo, const HostInfo& host, uint32_t now)
{
    std::string out;
    metric(out, "gpsdo_link_up", "Telemetry received from the GPSDO in the last 5 s", gpsdo.linkUp(now) ? 1 : 0);
    metric(out, "gpsdo_link_bad_checksums", "Sentences discarded since the ESP32 started", gpsdo.badChecksums());
    const Telemetry& t = gpsdo.telemetry();
    if (t.valid && gpsdo.linkUp(now)) {
        metric(out, "gpsdo_uptime_seconds", "GPSDO uptime", t.uptime);
        metric(out, "gpsdo_gps_lock", "GPS lock", t.gps_lock ? 1 : 0);
        metric(out, "gpsdo_ppb_lock", "PPB lock", t.ppb_lock ? 1 : 0);
        metric(out, "gpsdo_satellites", "Satellites used", t.sats);
        if (t.ppb_mean != PPB_UNKNOWN) {
            metric(out, "gpsdo_ppb_mean", "Mean frequency error over 128 s, in ppb", t.ppb_mean / 100.0);
        }
        metric(out, "gpsdo_ppb_instant", "Instant frequency error, in ppb", t.ppb_inst / 100.0);
        metric(out, "gpsdo_frequency_hz", "Measured frequency", t.freq);
        metric(out, "gpsdo_frequency_error_hz", "Frequency error", t.error);
        metric(out, "gpsdo_correction_steps", "Last PWM correction", t.correction);
        metric(out, "gpsdo_pwm", "OCXO control PWM", t.pwm);
        metric(out, "gpsdo_pps_shift_cycles", "PPS output shift, in clock cycles", t.pps_shift);
        metric(out, "gpsdo_pps_shift_seconds", "PPS output shift", t.pps_shift_ms * 1e-7);
        metric(out, "gpsdo_pps_sync_count", "PPS output resyncs", t.pps_sync_count);
        metric(out, "gpsdo_adjusting", "0 during warm-up", t.adjusting ? 1 : 0);
    }
    metric(out, "gpsdo_gps_satellites_in_view", "Satellites in view", gpsdo.gps().sat_count);
    metric(out, "gpsdo_esp_uptime_seconds", "ESP32 uptime", host.uptime_s);
    metric(out, "gpsdo_esp_free_heap_bytes", "ESP32 free heap", host.free_heap);
    metric(out, "gpsdo_esp_wifi_rssi_dbm", "WiFi signal", host.rssi);
    return out;
}
