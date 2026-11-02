// NMEA parsing for the data the GPSDO doesn't send in TLM. No Arduino dependency.
#pragma once

#include <cstddef>
#include <cstdint>

struct Satellite {
    char     talker[3];   // GP, BD, GL, GA...
    int      prn;
    int      elevation;   // degrees, -1 if unknown
    int      azimuth;     // degrees, -1 if unknown
    int      snr;         // dB-Hz, -1 when not tracked
    bool     used;
    uint32_t last_seen;
};

struct GpsData {
    static constexpr int MAX_SATS = 48;

    bool   has_time = false;
    char   time[9] = "";        // hh:mm:ss UTC
    bool   has_date = false;
    char   date[11] = "";       // YYYY-MM-DD
    int    fix_quality = 0;     // GGA: 0 none, 1 GPS, 2 DGPS...
    int    fix_mode = 1;        // GSA: 1 none, 2 2D, 3 3D
    bool   has_position = false;
    double latitude = 0;
    double longitude = 0;
    char   locator[9] = "";     // Maidenhead, 8 characters like the LCD
    bool   has_altitude = false;
    double altitude = 0;        // m above MSL
    double geoid = 0;           // m
    bool   has_hdop = false;
    double hdop = 0;
    int    sats_used = 0;       // From GGA, what the LCD and TLM show

    Satellite sats[MAX_SATS];
    int    sat_count = 0;

    // Takes a whole sentence whose checksum was verified, without CRLF
    void parse(const char* sentence, uint32_t now);
    // Forgets satellites no longer reported
    void expire(uint32_t now);

private:
    uint32_t last_gsa_ = 0;
    bool     have_gsa_ = false;
    void parseGga(char** f, int n);
    void parseRmc(char** f, int n);
    void parseGsa(const char* talker, char** f, int n, uint32_t now);
    void parseGsv(const char* talker, char** f, int n, uint32_t now);
    void clearUsed();
};

// ddmm.mmmm with hemisphere to signed decimal degrees. False if the field is empty.
bool nmeaParseCoordinate(const char* value, const char* hemisphere, double* out);
// Writes an 8 character Maidenhead locator
void maidenheadLocator(double latitude, double longitude, char out[9]);
