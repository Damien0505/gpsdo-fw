#include "nmea.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t SAT_EXPIRY_MS = 10000;
// All GSA sentences of a GPS epoch come together
constexpr uint32_t GSA_EPOCH_GAP_MS = 300;

constexpr int MAX_FIELDS = 24;

// Splits in place, keeping empty fields. The checksum, if any, is cut first.
int splitFields(char* line, char** fields)
{
    char* star = std::strrchr(line, '*');
    if (star != nullptr) {
        *star = '\0';
    }
    int n = 0;
    char* p = line;
    while (n < MAX_FIELDS) {
        fields[n++] = p;
        char* comma = std::strchr(p, ',');
        if (comma == nullptr) {
            break;
        }
        *comma = '\0';
        p = comma + 1;
    }
    return n;
}

bool isDigits(const char* s, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (s[i] < '0' || s[i] > '9') {
            return false;
        }
    }
    return true;
}

int intField(const char* s, int fallback)
{
    return *s != '\0' ? std::atoi(s) : fallback;
}

}  // namespace

bool nmeaParseCoordinate(const char* value, const char* hemisphere, double* out)
{
    const char* dot = std::strchr(value, '.');
    size_t int_len = dot != nullptr ? (size_t)(dot - value) : std::strlen(value);
    if (int_len < 3 || int_len > 5 || !isDigits(value, int_len)) {
        return false;
    }
    double raw = std::atof(value);
    double degrees = std::floor(raw / 100.0);
    double result = degrees + (raw - degrees * 100.0) / 60.0;
    if (hemisphere[0] == 'S' || hemisphere[0] == 'W') {
        result = -result;
    } else if (hemisphere[0] != 'N' && hemisphere[0] != 'E') {
        return false;
    }
    *out = result;
    return true;
}

void maidenheadLocator(double latitude, double longitude, char out[9])
{
    // Field 20x10 degrees, square 2x1, subsquare 5x2.5 minutes, extended square 30x15 seconds
    double lon = longitude + 180.0;
    double lat = latitude + 90.0;
    if (lon < 0) lon = 0;
    if (lon >= 360) lon = 359.999999;
    if (lat < 0) lat = 0;
    if (lat >= 180) lat = 179.999999;
    int a = (int)(lon / 20.0);
    int b = (int)(lat / 10.0);
    lon -= a * 20.0;
    lat -= b * 10.0;
    int c = (int)(lon / 2.0);
    int d = (int)lat;
    lon -= c * 2.0;
    lat -= d;
    int e = (int)(lon * 12.0);
    int f = (int)(lat * 24.0);
    lon -= e / 12.0;
    lat -= f / 24.0;
    int g = (int)(lon * 120.0);
    int h = (int)(lat * 240.0);
    if (g > 9) g = 9;
    if (h > 9) h = 9;
    out[0] = (char)('A' + a);
    out[1] = (char)('A' + b);
    out[2] = (char)('0' + c);
    out[3] = (char)('0' + d);
    out[4] = (char)('A' + e);
    out[5] = (char)('A' + f);
    out[6] = (char)('0' + g);
    out[7] = (char)('0' + h);
    out[8] = '\0';
}

void GpsData::parse(const char* sentence, uint32_t now)
{
    // $xxYYY with x the talker
    if (sentence[0] != '$' || std::strlen(sentence) < 6) {
        return;
    }
    char copy[128];
    size_t len = std::strlen(sentence);
    if (len >= sizeof(copy)) {
        return;
    }
    std::memcpy(copy, sentence, len + 1);

    char* f[MAX_FIELDS];
    int n = splitFields(copy, f);
    char talker[3] = { f[0][1], f[0][2], '\0' };
    const char* type = f[0] + 3;

    if (std::strcmp(type, "GGA") == 0) {
        parseGga(f, n);
    } else if (std::strcmp(type, "RMC") == 0) {
        parseRmc(f, n);
    } else if (std::strcmp(type, "GSA") == 0) {
        parseGsa(talker, f, n, now);
    } else if (std::strcmp(type, "GSV") == 0) {
        parseGsv(talker, f, n, now);
    }
}

static void formatTime(const char* field, char out[9], bool* has)
{
    if (std::strlen(field) >= 6 && isDigits(field, 6)) {
        std::snprintf(out, 9, "%.2s:%.2s:%.2s", field, field + 2, field + 4);
        *has = true;
    }
}

void GpsData::parseGga(char** f, int n)
{
    if (n < 12) {
        return;
    }
    formatTime(f[1], time, &has_time);
    fix_quality = intField(f[6], 0);
    sats_used = intField(f[7], 0);
    double lat, lon;
    if (nmeaParseCoordinate(f[2], f[3], &lat) && nmeaParseCoordinate(f[4], f[5], &lon)) {
        latitude = lat;
        longitude = lon;
        has_position = true;
        maidenheadLocator(lat, lon, locator);
    }
    if (*f[8] != '\0') {
        hdop = std::atof(f[8]);
        has_hdop = true;
    }
    if (*f[9] != '\0') {
        altitude = std::atof(f[9]);
        geoid = *f[11] != '\0' ? std::atof(f[11]) : 0;
        has_altitude = true;
    }
}

void GpsData::parseRmc(char** f, int n)
{
    if (n < 10) {
        return;
    }
    formatTime(f[1], time, &has_time);
    const char* d = f[9];
    if (std::strlen(d) == 6 && isDigits(d, 6)) {
        std::snprintf(date, sizeof(date), "20%.2s-%.2s-%.2s", d + 4, d + 2, d);
        has_date = true;
    }
}

void GpsData::clearUsed()
{
    for (int i = 0; i < sat_count; i++) {
        sats[i].used = false;
    }
}

void GpsData::parseGsa(const char* talker, char** f, int n, uint32_t now)
{
    if (n < 15) {
        return;
    }
    if (!have_gsa_ || (now - last_gsa_) > GSA_EPOCH_GAP_MS) {
        clearUsed();
    }
    have_gsa_ = true;
    last_gsa_ = now;
    fix_mode = intField(f[2], 1);

    // NMEA 4.1 adds a system ID, needed to tell constellations apart under the GN talker
    const char* system = nullptr;
    if (n >= 19 && f[18][0] != '\0' && f[18][1] == '\0') {
        switch (f[18][0]) {
            case '1': system = "GP"; break;
            case '2': system = "GL"; break;
            case '3': system = "GA"; break;
            case '4': system = "BD"; break;
            default: break;
        }
    }
    if (system == nullptr && std::strcmp(talker, "GN") != 0) {
        system = talker;
    }
    for (int i = 3; i < 15; i++) {
        int prn = intField(f[i], 0);
        if (prn <= 0) {
            continue;
        }
        for (int s = 0; s < sat_count; s++) {
            if (sats[s].prn == prn && (system == nullptr || std::strcmp(sats[s].talker, system) == 0)) {
                sats[s].used = true;
            }
        }
    }
}

void GpsData::parseGsv(const char* talker, char** f, int n, uint32_t now)
{
    if (n < 4) {
        return;
    }
    // Groups of 4 fields, possibly followed by a signal ID: a field count that doesn't matter
    for (int i = 4; i + 3 < n; i += 4) {
        int prn = intField(f[i], 0);
        if (prn <= 0) {
            continue;
        }
        Satellite* sat = nullptr;
        for (int s = 0; s < sat_count; s++) {
            if (sats[s].prn == prn && std::strcmp(sats[s].talker, talker) == 0) {
                sat = &sats[s];
                break;
            }
        }
        if (sat == nullptr) {
            if (sat_count >= MAX_SATS) {
                continue;
            }
            sat = &sats[sat_count++];
            std::strcpy(sat->talker, talker);
            sat->prn = prn;
            sat->used = false;
        }
        sat->elevation = intField(f[i + 1], -1);
        sat->azimuth = intField(f[i + 2], -1);
        sat->snr = intField(f[i + 3], -1);
        sat->last_seen = now;
    }
}

void GpsData::expire(uint32_t now)
{
    int kept = 0;
    for (int s = 0; s < sat_count; s++) {
        if ((now - sats[s].last_seen) <= SAT_EXPIRY_MS) {
            if (kept != s) {
                sats[kept] = sats[s];
            }
            kept++;
        }
    }
    sat_count = kept;
}
