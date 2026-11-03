// PC tests for the ESP32 core logic. Usage: test_core <simulator transcript> <corrupted transcript>
#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gpsdo.h"
#include "reports.h"

static int checks, failures;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    std::printf("  FAIL %s:%d: %s\n", __func__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a), _b = (long long)(b); checks++; if (_a != _b) { failures++; \
    std::printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __func__, __LINE__, #a, _a, _b); } } while (0)
#define CHECK_STR(a, b) do { std::string _a = (a), _b = (b); checks++; if (_a != _b) { failures++; \
    std::printf("  FAIL %s:%d: %s = '%s', expected '%s'\n", __func__, __LINE__, #a, _a.c_str(), _b.c_str()); } } while (0)
#define CHECK_NEAR(a, b, tol) do { double _a = (a), _b = (b); checks++; if (_a < _b - (tol) || _a > _b + (tol)) { failures++; \
    std::printf("  FAIL %s:%d: %s = %f, expected %f\n", __func__, __LINE__, #a, _a, _b); } } while (0)

static std::string sentence(const std::string& body)
{
    unsigned char cs = 0;
    for (char c : body) cs ^= (unsigned char)c;
    char tail[8];
    std::snprintf(tail, sizeof(tail), "*%02X\r\n", cs);
    return "$" + body + tail;
}

struct Rig {
    std::vector<std::string> sent;
    Gpsdo gpsdo;
    uint32_t now = 1000;
    Rig() : gpsdo([this](const char* d, size_t n) { sent.emplace_back(d, n); }) {}
    void feed(const std::string& text) { gpsdo.onBytes((const uint8_t*)text.data(), text.size(), now); }
    void line(const std::string& body) { feed(sentence(body)); }
    void tlm(uint32_t uptime = 100) {
        line("PGPSDO,TLM," + std::to_string(uptime) + ",1,1,9,3,-12,70000000,0,-1,38212,0,12,0,4,1");
    }
    void advance(uint32_t ms) { for (uint32_t i = 0; i < ms; i += 10) { now += 10; gpsdo.tick(now); } }
    // Starts up and answers what the core asks at boot
    void ready() {
        gpsdo.tick(now);
        tlm(); answerAll();
        gpsdo.tick(now);
        answerAll();
    }
    void answerAll() {
        for (int guard = 0; guard < 8; guard++) {
            if (sent.empty()) return;
            std::string s = sent.front();
            sent.erase(sent.begin());
            if (s.find("GET,VER") != std::string::npos) line("PGPSDO,ACK,GET,VER,OK");
            else if (s.find("GET,CFG") != std::string::npos) {
                line("PGPSDO,ACK,GET,CFG,OK");
                line("PGPSDO,CFG,contrast=80,fw_version=v1");
            } else return;
        }
    }
};

static void test_examples_from_protocol_document()
{
    Rig r;
    r.feed("$PGPSDO,VER,1,v0.1.16-web1*5F\r\n");
    CHECK_STR(r.gpsdo.firmwareVersion(), "v0.1.16-web1");
    CHECK_EQ(r.gpsdo.protocolVersion(), 1);
    r.feed("$PGPSDO,TLM,1234,1,1,9,3,-12,70000000,0,-1,38212,0,12,0,4,1*7D\r\n");
    const Telemetry& t = r.gpsdo.telemetry();
    CHECK_EQ(r.gpsdo.badChecksums(), 0);
    CHECK(t.valid);
    CHECK_EQ(t.uptime, 1234); CHECK(t.gps_lock); CHECK(t.ppb_lock); CHECK_EQ(t.sats, 9);
    CHECK_EQ(t.ppb_mean, 3); CHECK_EQ(t.ppb_inst, -12); CHECK_EQ(t.freq, 70000000); CHECK_EQ(t.error, 0);
    CHECK_EQ(t.correction, -1); CHECK_EQ(t.pwm, 38212); CHECK_EQ(t.millis, 0); CHECK_EQ(t.pps_shift, 12);
    CHECK_EQ(t.pps_shift_ms, 0); CHECK_EQ(t.pps_sync_count, 4); CHECK(t.adjusting);

    // What the core sends, byte for byte
    r.gpsdo.tick(r.now);
    r.answerAll();
    auto a = r.gpsdo.set("pps_sync_delay", 30, r.now);
    CHECK_STR(r.sent.back(), "$PGPSDO,SET,pps_sync_delay,30*73\r\n");
    r.feed("$PGPSDO,ACK,SET,pps_sync_delay,OK*11\r\n");
    CHECK(a->state == CommandResult::OK);
    auto b = r.gpsdo.command("SAVEPWM", r.now);
    CHECK_STR(r.sent.back(), "$PGPSDO,CMD,SAVEPWM*1E\r\n");
    r.feed("$PGPSDO,ACK,CMD,SAVEPWM,OK*53\r\n");
    CHECK(b->state == CommandResult::OK);
    auto c = r.gpsdo.command("FORCESYNC", r.now);
    CHECK_STR(r.sent.back(), "$PGPSDO,CMD,FORCESYNC*0F\r\n");
    r.line("PGPSDO,ACK,CMD,FORCESYNC,OK");
    auto d = r.gpsdo.set("contrast", 101, r.now);
    r.feed("$PGPSDO,ACK,SET,contrast,ERR,RANGE*20\r\n");
    CHECK(d->state == CommandResult::FAILED);
    CHECK_STR(d->error, "RANGE");
}

static void test_bad_input()
{
    Rig r;
    r.feed("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00\r\n");
    CHECK_EQ(r.gpsdo.badChecksums(), 1);
    CHECK(!r.gpsdo.gps().has_position);
    r.feed("garbage\r\n");
    CHECK_EQ(r.gpsdo.badChecksums(), 2);
    r.feed("$GPGGA,no star\r\n");
    CHECK_EQ(r.gpsdo.badChecksums(), 3);
    // Line over the limit, then a good one
    r.feed(std::string(500, 'x') + "\n");
    CHECK_EQ(r.gpsdo.badChecksums(), 4);
    r.tlm();
    CHECK(r.gpsdo.telemetry().valid);
    // Binary with zeros
    std::string binary("\xB5\x62\x00\x01\x00\n", 6);
    r.feed(binary);
    CHECK_EQ(r.gpsdo.badChecksums(), 5);
    // TLM too short or with a broken field: discarded and counted
    uint32_t before = r.gpsdo.telemetry().uptime;
    r.line("PGPSDO,TLM,999,1,1");
    CHECK_EQ(r.gpsdo.telemetry().uptime, before);
    CHECK_EQ(r.gpsdo.badChecksums(), 6);
    r.line("PGPSDO,TLM,999,1,1,9,x,-12,70000000,0,-1,38212,0,12,0,4,1");
    CHECK_EQ(r.gpsdo.telemetry().uptime, before);
    // Later versions may append fields
    r.line("PGPSDO,TLM,1000,1,1,9,3,-12,70000000,0,-1,38212,0,12,0,4,1,77,88");
    CHECK_EQ(r.gpsdo.telemetry().uptime, 1000);
    // Unknown sentence types are ignored
    r.line("PGPSDO,NEW,1,2,3");
    // Split in the middle of a line, as UART reads do
    r.feed("$PGPSDO,VER,1,");
    r.feed("v9*");
    std::string whole = sentence("PGPSDO,VER,1,v9");
    r.feed(whole.substr(whole.size() - 4));
    CHECK_STR(r.gpsdo.firmwareVersion(), "v9");
}

static void test_link_state()
{
    Rig r;
    CHECK(!r.gpsdo.linkUp(r.now));
    r.ready();
    CHECK(r.gpsdo.linkUp(r.now));
    r.advance(4900);
    CHECK(r.gpsdo.linkUp(r.now));
    r.advance(200);
    CHECK(!r.gpsdo.linkUp(r.now));
    CHECK_EQ(r.gpsdo.linkDownEvents(), 1);
    // NMEA only is not enough: it's telemetry that tells the GPSDO is alive
    r.line("GPGSA,A,1,,,,,,,,,,,,,,,");
    CHECK(!r.gpsdo.linkUp(r.now));
    r.tlm(200);
    CHECK(r.gpsdo.linkUp(r.now));
}

static void test_config()
{
    Rig r;
    r.gpsdo.tick(r.now);
    r.tlm();
    // Exactly what the real firmware's host test prints
    r.line("PGPSDO,CFG,contrast=80,pwm_auto_save=0,pps_auto_sync=1,ppb_lock_threshold=50,ocxo_model=0,warmup_time=70,correction_algo=2,correction_factor=300");
    CHECK(!r.gpsdo.configComplete());
    CHECK(!r.gpsdo.takeConfigChanged());
    r.line("PGPSDO,CFG,gps_time_offset=-5,comm_baudrate=115200,gps_model=3,fw_version=v0.1.16-web1");
    CHECK(r.gpsdo.configComplete());
    CHECK(r.gpsdo.takeConfigChanged());
    CHECK(!r.gpsdo.takeConfigChanged());
    CHECK_STR(r.gpsdo.config().get("contrast"), "80");
    CHECK_STR(r.gpsdo.config().get("gps_time_offset"), "-5");
    CHECK(r.gpsdo.config().get("nope") == nullptr);
    // Encoder change: same keys are updated, not added
    int keys = r.gpsdo.config().size();
    r.line("PGPSDO,CFG,contrast=55,fw_version=v0.1.16-web1");
    CHECK_EQ(r.gpsdo.config().size(), keys);
    CHECK_STR(r.gpsdo.config().get("contrast"), "55");
    CHECK(r.gpsdo.takeConfigChanged());

    JsonDocument doc;
    CHECK(deserializeJson(doc, configJson(r.gpsdo)) == DeserializationError::Ok);
    CHECK_EQ(doc["config"]["contrast"].as<int>(), 55);
    CHECK_EQ(doc["config"]["gps_time_offset"].as<int>(), -5);
    CHECK_EQ(doc["config"]["comm_baudrate"].as<long>(), 115200);
    CHECK(doc["config"]["fw_version"].isNull());
    CHECK_STR(doc["fw_version"].as<const char*>(), "v0.1.16-web1");
    CHECK(doc["complete"].as<bool>());
}

static void test_boot_conversation()
{
    Rig r;
    r.gpsdo.tick(r.now);
    CHECK_EQ(r.sent.size(), 1);
    CHECK(r.sent[0].find("GET,VER") != std::string::npos);
    // Asks settings once the GPSDO shows up, and insists until it gets them
    r.tlm();
    r.line("PGPSDO,ACK,GET,VER,OK");
    r.gpsdo.tick(r.now);
    CHECK(r.sent.back().find("GET,CFG") != std::string::npos);
    r.line("PGPSDO,ACK,GET,CFG,OK");
    r.sent.clear();
    for (int i = 0; i < 6; i++) { r.advance(1000); r.tlm(101 + i); }
    CHECK(!r.sent.empty());
    CHECK(r.sent.back().find("GET,CFG") != std::string::npos);
    r.line("PGPSDO,ACK,GET,CFG,OK");
    r.line("PGPSDO,CFG,contrast=80,fw_version=v1");
    r.sent.clear();
    for (int i = 0; i < 20; i++) { r.advance(1000); r.tlm(110 + i); }
    CHECK(r.sent.empty());
    // GPSDO rebooted: asks again
    r.tlm(3);
    CHECK(!r.gpsdo.configComplete());
    CHECK(!r.sent.empty());
    CHECK(r.sent.back().find("GET,CFG") != std::string::npos);
    // Also on its boot announcement
    r.line("PGPSDO,ACK,GET,CFG,OK");
    r.line("PGPSDO,CFG,fw_version=v1");
    r.line("PGPSDO,ACK,GET,VER,OK");
    r.sent.clear();
    r.line("PGPSDO,VER,1,v1");
    CHECK(!r.sent.empty());
}

static void test_commands()
{
    Rig r;
    r.ready();
    r.sent.clear();

    // One at a time
    auto a = r.gpsdo.set("contrast", 1, r.now);
    auto b = r.gpsdo.set("contrast", 2, r.now);
    CHECK_EQ(r.sent.size(), 1);
    CHECK(a->state == CommandResult::PENDING);
    r.line("PGPSDO,ACK,SET,contrast,OK");
    CHECK(a->state == CommandResult::OK);
    CHECK_EQ(r.sent.size(), 2);
    CHECK(r.sent[1].find("contrast,2") != std::string::npos);
    // A reply for something else doesn't finish it
    r.line("PGPSDO,ACK,SET,other,OK");
    r.line("PGPSDO,ACK,CMD,contrast,OK");
    CHECK(b->state == CommandResult::PENDING);
    r.line("PGPSDO,ACK,SET,contrast,ERR,RANGE");
    CHECK(b->state == CommandResult::FAILED);
    CHECK_STR(b->error, "RANGE");

    // No ACK: one retry after 2 s, gives up after 2 s more
    r.tlm(); r.sent.clear();
    auto c = r.gpsdo.command("FORCESYNC", r.now);
    CHECK_EQ(r.sent.size(), 1);
    r.advance(1900); CHECK_EQ(r.sent.size(), 1);
    r.tlm();
    r.advance(200); CHECK_EQ(r.sent.size(), 2);
    CHECK(r.sent[0] == r.sent[1]);
    CHECK(c->state == CommandResult::PENDING);
    r.tlm();
    r.advance(2000);
    CHECK(c->state == CommandResult::FAILED);
    CHECK_STR(c->error, "TIMEOUT");
    CHECK_EQ(r.sent.size(), 2);

    // Answer to the retry counts
    r.tlm(); r.sent.clear();
    auto d = r.gpsdo.set("contrast", 3, r.now);
    r.tlm(); r.advance(2100); r.tlm();
    r.line("PGPSDO,ACK,SET,contrast,OK");
    CHECK(d->state == CommandResult::OK);

    // Damaged on the way: sent again at once, then given up on
    r.tlm(); r.sent.clear();
    auto e = r.gpsdo.set("contrast", 4, r.now);
    r.line("PGPSDO,ACK,ERR,CHECKSUM");
    CHECK_EQ(r.sent.size(), 2);
    CHECK(e->state == CommandResult::PENDING);
    r.line("PGPSDO,ACK,ERR,CHECKSUM");
    CHECK(e->state == CommandResult::FAILED);
    CHECK_STR(e->error, "CHECKSUM");

    // A bad reply: the checksum fails, it's as if nothing came
    r.sent.clear();
    auto f = r.gpsdo.set("contrast", 5, r.now);
    r.feed("$PGPSDO,ACK,SET,contrast,OK*00\r\n");
    CHECK(f->state == CommandResult::PENDING);
    r.line("PGPSDO,ACK,SET,contrast,OK");

    // Link down: fails at once
    r.advance(6000);
    auto g = r.gpsdo.set("contrast", 6, r.now);
    CHECK(g->state == CommandResult::FAILED);
    CHECK_STR(g->error, "LINK_DOWN");

    // Queue limit
    r.tlm(); r.sent.clear();
    std::vector<CommandResultPtr> all;
    for (size_t i = 0; i < Gpsdo::MAX_QUEUED_COMMANDS + 3; i++) all.push_back(r.gpsdo.set("contrast", (long long)i, r.now));
    CHECK(all.back()->state == CommandResult::FAILED);
    CHECK_STR(all.back()->error, "BUSY");
    // Queue drains
    for (int i = 0; i < 40; i++) { r.tlm(); r.line("PGPSDO,ACK,SET,contrast,OK"); }
    CHECK(all[Gpsdo::MAX_QUEUED_COMMANDS - 1]->state == CommandResult::OK);

    // Negative values
    r.sent.clear();
    r.gpsdo.set("gps_time_offset", -14, r.now);
    CHECK(r.sent.back().find(",gps_time_offset,-14*") != std::string::npos);
}

static void test_gps()
{
    Rig r;
    const GpsData& g = r.gpsdo.gps();
    // No fix yet: empty fields everywhere
    r.line("GNGGA,,,,,,0,00,99.99,,,,,,");
    r.line("GNRMC,,V,,,,,,,,,,N");
    r.line("GPGSA,A,1,,,,,,,,,,,,,99.99,99.99,99.99");
    r.line("GPGSV,1,1,00");
    CHECK(!g.has_position); CHECK(!g.has_time); CHECK(!g.has_date); CHECK(!g.has_altitude);
    CHECK_EQ(g.fix_quality, 0);
    CHECK_EQ(g.sat_count, 0);

    r.line("GNGGA,123519.00,4807.03800,N,01131.00000,E,1,08,0.9,545.4,M,46.9,M,,");
    CHECK_STR(g.time, "12:35:19");
    CHECK_NEAR(g.latitude, 48.1173, 0.0001);
    CHECK_NEAR(g.longitude, 11.5167, 0.0001);
    CHECK_STR(std::string(g.locator, 6), "JN58SC");
    CHECK_NEAR(g.altitude, 545.4, 0.01);
    CHECK_NEAR(g.geoid, 46.9, 0.01);
    CHECK_NEAR(g.hdop, 0.9, 0.01);
    CHECK_EQ(g.sats_used, 8);
    r.line("GNRMC,123520.00,A,4807.038,N,01131.000,E,0.1,84.4,230394,,,A");
    CHECK_STR(g.date, "2094-03-23");
    CHECK_STR(g.time, "12:35:20");
    // Southern and western
    r.line("GNGGA,000000.00,3351.65000,S,15112.58000,E,1,08,0.9,58,M,22,M,,");
    CHECK_NEAR(g.latitude, -33.8608, 0.0001);
    CHECK_NEAR(g.longitude, 151.2097, 0.0001);
    CHECK_STR(std::string(g.locator, 4), "QF56");
    r.line("GNGGA,000000.00,3351.65000,N,07400.00000,W,1,08,0.9,58,M,22,M,,");
    CHECK_NEAR(g.longitude, -74.0, 0.0001);
    char loc[9];
    maidenheadLocator(-90, -180, loc); CHECK_STR(loc, "AA00AA00");
    maidenheadLocator(89.9999, 179.9999, loc); CHECK_EQ(loc[0], 'R'); CHECK_EQ(loc[1], 'R');

    // Satellites across constellations, same PRN on two of them
    r.line("GPGSV,2,1,06,04,62,123,43,09,45,233,42,16,33,054,43,27,71,310,40");
    r.line("GPGSV,2,2,06,08,12,178,,30,05,288,");
    r.line("BDGSV,1,1,02,04,55,040,34,11,40,201,31");
    CHECK_EQ(g.sat_count, 8);
    r.line("GPGSA,A,3,04,09,,,,,,,,,,,1.9,1.1,1.5");
    int used_gp = 0, used_bd = 0, untracked = 0;
    for (int i = 0; i < g.sat_count; i++) {
        if (g.sats[i].used && std::string(g.sats[i].talker) == "GP") used_gp++;
        if (g.sats[i].used && std::string(g.sats[i].talker) == "BD") used_bd++;
        if (g.sats[i].snr < 0) untracked++;
    }
    CHECK_EQ(used_gp, 2);
    CHECK_EQ(used_bd, 0);
    CHECK_EQ(untracked, 2);
    CHECK_EQ(g.fix_mode, 3);
    // Same epoch: adds. Later epoch: replaces.
    r.line("BDGSA,A,3,11,,,,,,,,,,,,1.9,1.1,1.5");
    used_gp = used_bd = 0;
    for (int i = 0; i < g.sat_count; i++) {
        if (g.sats[i].used && std::string(g.sats[i].talker) == "GP") used_gp++;
        if (g.sats[i].used && std::string(g.sats[i].talker) == "BD") used_bd++;
    }
    CHECK_EQ(used_gp, 2); CHECK_EQ(used_bd, 1);
    r.now += 1000;
    r.line("GPGSA,A,3,16,,,,,,,,,,,,1.9,1.1,1.5");
    used_gp = used_bd = 0;
    for (int i = 0; i < g.sat_count; i++) {
        if (g.sats[i].used) used_gp++;
    }
    CHECK_EQ(used_gp, 1);
    // NMEA 4.1 with a system ID, GN talker
    r.now += 1000;
    r.line("GNGSA,A,3,04,,,,,,,,,,,,1.9,1.1,1.5,4");
    for (int i = 0; i < g.sat_count; i++) {
        if (g.sats[i].prn == 4) CHECK_EQ(g.sats[i].used, std::string(g.sats[i].talker) == "BD");
    }
    // Refresh in place, and forget after a while
    r.line("GPGSV,1,1,01,09,50,240,38");
    int count = g.sat_count;
    CHECK_EQ(count, 8);
    r.advance(10100);
    r.gpsdo.tick(r.now);
    CHECK_EQ(g.sat_count, 0);
    // More than the table holds: no overflow
    for (int i = 0; i < 40; i++) {
        char body[80];
        std::snprintf(body, sizeof(body), "GPGSV,9,1,99,%d,1,1,1,%d,1,1,1,%d,1,1,1,%d,1,1,1", i * 4 + 1, i * 4 + 2, i * 4 + 3, i * 4 + 4);
        r.line(body);
    }
    CHECK_EQ(g.sat_count, GpsData::MAX_SATS);
    // Ignored
    r.line("GNZDA,123519.00,23,03,2094,00,00");
    r.line("GNVTG,0.00,T,,M,0.01,N,0.02,K,A");
    r.line("GPTXT,01,01,02,ANTENNA OK");
}

static std::string readAll(const History& h, History::Range range, size_t chunk)
{
    auto reader = h.startReader(range);
    std::string out;
    std::vector<char> buffer(chunk);
    for (;;) {
        size_t n = h.read(reader, buffer.data(), chunk);
        if (n == 0) return out;
        out.append(buffer.data(), n);
    }
}

static History history;  // Too large for the stack

static void test_history()
{
    history = History();
    JsonDocument doc;
    CHECK(deserializeJson(doc, readAll(history, History::RANGE_1H, 256)) == DeserializationError::Ok);
    CHECK_EQ(doc["count"].as<int>(), 0);

    for (int i = 0; i < 90; i++) history.addValue(i);
    CHECK(deserializeJson(doc, readAll(history, History::RANGE_1H, 64)) == DeserializationError::Ok);
    CHECK_EQ(doc["count"].as<int>(), 90);
    CHECK_EQ(doc["step_s"].as<int>(), 1);
    CHECK_EQ(doc["rows"][0].as<int>(), 0);
    CHECK_EQ(doc["rows"][89].as<int>(), 89);
    CHECK_EQ(doc["rows"].size(), 90);

    // First complete minute
    CHECK(deserializeJson(doc, readAll(history, History::RANGE_24H, 256)) == DeserializationError::Ok);
    CHECK_EQ(doc["count"].as<int>(), 1);
    CHECK_EQ(doc["step_s"].as<int>(), 60);
    CHECK_EQ(doc["rows"][0][0].as<int>(), 0);
    CHECK_EQ(doc["rows"][0][1].as<int>(), 29);
    CHECK_EQ(doc["rows"][0][2].as<int>(), 59);

    // Gaps: null, and not counted in a minute's mean
    history = History();
    for (int i = 0; i < 30; i++) history.addGap();
    for (int i = 0; i < 30; i++) history.addValue(-100);
    CHECK(deserializeJson(doc, readAll(history, History::RANGE_1H, 128)) == DeserializationError::Ok);
    CHECK(doc["rows"][0].isNull());
    CHECK_EQ(doc["rows"][30].as<int>(), -100);
    CHECK(deserializeJson(doc, readAll(history, History::RANGE_24H, 128)) == DeserializationError::Ok);
    CHECK_EQ(doc["rows"][0][1].as<int>(), -100);
    for (int i = 0; i < 60; i++) history.addGap();
    CHECK(deserializeJson(doc, readAll(history, History::RANGE_24H, 128)) == DeserializationError::Ok);
    CHECK(doc["rows"][1][1].isNull());

    // Wraps around, large values, whatever the chunk size
    history = History();
    long total = (long)History::COARSE_SIZE * 60 + 90;
    for (long i = 0; i < total; i++) history.addValue(i % 3 == 0 ? -2000000000 : 2000000000);
    CHECK_EQ(history.fineCount(), History::FINE_SIZE);
    CHECK_EQ(history.coarseCount(), History::COARSE_SIZE);
    for (size_t chunk : { (size_t)64, (size_t)100, (size_t)1460 }) {
        std::string all = readAll(history, History::RANGE_1H, chunk);
        JsonDocument big;
        CHECK(deserializeJson(big, all) == DeserializationError::Ok);
        CHECK_EQ(big["rows"].size(), History::FINE_SIZE);
        std::string coarse = readAll(history, History::RANGE_24H, chunk);
        CHECK(deserializeJson(big, coarse) == DeserializationError::Ok);
        CHECK_EQ(big["rows"].size(), History::COARSE_SIZE);
        CHECK_EQ(big["rows"][0][0].as<long>(), -2000000000);
        CHECK_EQ(big["rows"][0][2].as<long>(), 2000000000);
    }
    // Oldest first, after wrapping
    history = History();
    for (long i = 0; i < History::FINE_SIZE + 5; i++) history.addValue((int32_t)i);
    JsonDocument last;
    deserializeJson(last, readAll(history, History::RANGE_1H, 256));
    CHECK_EQ(last["rows"][0].as<int>(), 5);
    CHECK_EQ(last["rows"][History::FINE_SIZE - 1].as<int>(), History::FINE_SIZE + 4);
}

static void test_history_from_link()
{
    static Rig r;
    r.gpsdo.tick(r.now);
    // TLM every second: one value each
    for (int i = 0; i < 10; i++) { r.tlm(100 + i); r.advance(1000); }
    CHECK_EQ(r.gpsdo.history().fineCount(), 10);
    // No measurement yet: gap, not 655.35 ppb
    r.line("PGPSDO,TLM,120,1,1,9,65535,-12,70000000,0,-1,38212,0,12,0,4,1");
    r.advance(1000);
    r.tlm(121);
    // Outage: time scale kept
    uint32_t before = r.gpsdo.history().fineCount();
    r.advance(100000);
    uint32_t added = r.gpsdo.history().fineCount() - before;
    CHECK(added >= 98 && added <= 100);
    JsonDocument doc;
    std::string json;
    auto reader = r.gpsdo.history().startReader(History::RANGE_1H);
    char buffer[512];
    for (size_t n; (n = r.gpsdo.history().read(reader, buffer, sizeof(buffer))) != 0;) json.append(buffer, n);
    CHECK(deserializeJson(doc, json) == DeserializationError::Ok);
    CHECK_EQ(doc["rows"][0].as<int>(), 3);
    CHECK(doc["rows"][10].isNull());
    CHECK(doc["rows"][50].isNull());
}

static void test_reports()
{
    static Rig r;
    HostInfo host;
    host.uptime_s = 77; host.free_heap = 123456; host.rssi = -60; host.ip = "10.0.0.2"; host.hostname = "gpsdo";

    JsonDocument doc;
    r.gpsdo.tick(r.now);
    CHECK(deserializeJson(doc, statusJson(r.gpsdo, host, r.now)) == DeserializationError::Ok);
    CHECK(doc["tlm"].isNull());
    CHECK(!doc["link"]["up"].as<bool>());
    CHECK(doc["link"]["age_ms"].isNull());
    CHECK(doc["gps"]["time"].isNull());

    r.line("PGPSDO,TLM,1234,1,0,9,65535,-12,70000000,-1,-1,38212,0,12,99,4,1");
    r.line("GNGGA,123519.00,4807.03800,N,01131.00000,E,1,08,0.9,545.4,M,46.9,M,,");
    r.line("GPGSV,1,1,01,09,50,240,38");
    r.now += 300;
    CHECK(deserializeJson(doc, statusJson(r.gpsdo, host, r.now)) == DeserializationError::Ok);
    CHECK(doc["link"]["up"].as<bool>());
    CHECK_EQ(doc["link"]["age_ms"].as<int>(), 300);
    CHECK_EQ(doc["tlm"]["uptime"].as<int>(), 1234);
    CHECK(doc["tlm"]["ppb_mean"].isNull());
    CHECK_EQ(doc["tlm"]["ppb_inst"].as<int>(), -12);
    CHECK_EQ(doc["tlm"]["pps_shift_ms"].as<int>(), 99);
    CHECK(doc["tlm"]["gps_lock"].as<bool>());
    CHECK(!doc["tlm"]["ppb_lock"].as<bool>());
    CHECK_STR(doc["gps"]["time"].as<const char*>(), "12:35:19");
    CHECK_NEAR(doc["gps"]["lat"].as<double>(), 48.1173, 0.0001);
    CHECK_EQ(doc["gps"]["sats_view"].as<int>(), 1);
    CHECK_STR(doc["gps"]["sats"][0][0].as<const char*>(), "GP");
    CHECK_EQ(doc["gps"]["sats"][0][1].as<int>(), 9);
    CHECK_EQ(doc["gps"]["sats"][0][4].as<int>(), 38);
    CHECK_EQ(doc["esp"]["heap"].as<int>(), 123456);

    std::string metrics = metricsText(r.gpsdo, host, r.now);
    CHECK(metrics.find("gpsdo_link_up 1\n") != std::string::npos);
    CHECK(metrics.find("gpsdo_ppb_mean") == std::string::npos);  // Unknown yet
    CHECK(metrics.find("# TYPE gpsdo_pwm gauge\ngpsdo_pwm 38212\n") != std::string::npos);
    CHECK(metrics.find("gpsdo_frequency_hz 70000000\n") != std::string::npos);
    CHECK(metrics.find("gpsdo_pps_shift_seconds 9.9e-06\n") != std::string::npos);
    r.line("PGPSDO,TLM,1235,1,0,9,-250,-12,70000000,-1,-1,38212,0,12,99,4,1");
    metrics = metricsText(r.gpsdo, host, r.now);
    CHECK(metrics.find("gpsdo_ppb_mean -2.5\n") != std::string::npos);
    // Link gone: no stale values
    r.advance(6000);
    metrics = metricsText(r.gpsdo, host, r.now);
    CHECK(metrics.find("gpsdo_link_up 0\n") != std::string::npos);
    CHECK(metrics.find("gpsdo_pwm") == std::string::npos);
    CHECK(metrics.find("gpsdo_esp_uptime_seconds 77\n") != std::string::npos);
}

static void test_version_asked_again_when_link_comes_up()
{
    Rig r;
    r.gpsdo.tick(r.now);    // Asked at start, while the GPSDO isn't listening
    r.advance(10000);
    CHECK_EQ(r.gpsdo.protocolVersion(), 0);
    r.sent.clear();
    r.tlm();
    r.gpsdo.tick(r.now);
    bool asked = false;
    for (int i = 0; i < 4 && !asked; i++) {
        for (const std::string& s : r.sent) asked = asked || s.find("GET,VER") != std::string::npos;
        if (!r.sent.empty()) {
            std::string s = r.sent.front();
            r.sent.erase(r.sent.begin());
            if (s.find("GET,CFG") != std::string::npos) r.line("PGPSDO,ACK,GET,CFG,OK");
            r.gpsdo.tick(r.now);
        }
    }
    CHECK(asked);
    r.feed("$PGPSDO,VER,1,v0.1.16-web1*5F\r\n");
    CHECK_EQ(r.gpsdo.protocolVersion(), 1);
}

static std::string tlmWith(long long freq, long long shift)
{
    return "PGPSDO,TLM,100,1,1,9,3,-12," + std::to_string(freq) + ",0,0,38212,0," + std::to_string(shift) + ",7,4,1";
}

static void test_frequency_glitches()
{
    Rig r;
    r.line(tlmWith(70000001, 500));
    CHECK_EQ(r.gpsdo.telemetry().freq, 70000001);
    // As measured on a real unit, during a flash write
    r.line(tlmWith(68558207, -1404660));
    CHECK_EQ(r.gpsdo.telemetry().freq, 70000001);
    CHECK_EQ(r.gpsdo.telemetry().pps_shift, 500);
    CHECK_EQ(r.gpsdo.measurementGlitches(), 1);
    r.line(tlmWith(70000000, 600));
    CHECK_EQ(r.gpsdo.telemetry().freq, 70000000);
    CHECK_EQ(r.gpsdo.telemetry().pps_shift, 600);
    // One overflow out, then back: both held
    r.line(tlmWith(69934464, 700));
    r.line(tlmWith(70065535, 800));
    CHECK_EQ(r.gpsdo.telemetry().freq, 70000000);
    CHECK_EQ(r.gpsdo.measurementGlitches(), 3);
    // Normal drift is not a glitch, up to the firmware's limit
    r.line(tlmWith(70002000, 900));
    CHECK_EQ(r.gpsdo.telemetry().freq, 70002000);
    // A real fault must not stay hidden
    for (int i = 0; i < 3; i++) r.line(tlmWith(69000000, 1));
    CHECK_EQ(r.gpsdo.telemetry().freq, 70002000);
    r.line(tlmWith(69000000, 1));
    CHECK_EQ(r.gpsdo.telemetry().freq, 69000000);
    // Nothing to hold on to yet
    Rig first;
    first.line(tlmWith(69000000, 1));
    CHECK_EQ(first.gpsdo.telemetry().freq, 69000000);
    CHECK_EQ(first.gpsdo.measurementGlitches(), 0);
}

static std::string cfgFrame(uint8_t id, const std::vector<uint8_t>& payload, uint32_t extra = 0)
{
    uint32_t sum = (0x06u << 24) + ((uint32_t)id << 16) + (uint32_t)payload.size() + extra;
    for (size_t i = 0; i + 4 <= payload.size(); i += 4) {
        sum += (uint32_t)payload[i] | (uint32_t)payload[i + 1] << 8 | (uint32_t)payload[i + 2] << 16 | (uint32_t)payload[i + 3] << 24;
    }
    std::string f;
    f += (char)0xBA; f += (char)0xCE;
    f += (char)payload.size(); f += '\0';
    f += (char)0x06; f += (char)id;
    for (uint8_t b : payload) f += (char)b;
    for (int s = 0; s < 32; s += 8) f += (char)(sum >> s);
    return f;
}

static void test_gps_query()
{
    // From the specification: empty CFG message, checksum (class << 24) + (id << 16) + length
    const char expected[] = "\xBA\xCE\x00\x00\x06\x03\x00\x00\x03\x06\r\n";
    CHECK(GpsQuery::binaryQuery(0x06, 0x03) == std::string(expected, sizeof(expected) - 1));
    // Examples given by the specification
    CHECK_STR(GpsQuery::textQuery("PCAS03,1,1,1,1,1,1,0,1"), "$PCAS03,1,1,1,1,1,1,0,1*03\r\n");
    CHECK_STR(GpsQuery::textQuery("PCAS00"), "$PCAS00*01\r\n");

    Rig r;
    r.ready();
    r.sent.clear();
    // Only reads: nothing goes out unless asked
    r.advance(5000);
    CHECK_EQ(r.sent.size(), 0);
    CHECK(!r.gpsdo.gpsQuery().finished());

    r.gpsdo.startGpsQuery(r.now);
    r.advance(10);
    CHECK_EQ(r.sent.size(), 1);
    CHECK_STR(r.sent[0], "$PCAS06,0*1B\r\n");
    r.gpsdo.startGpsQuery(r.now);   // A second request doesn't restart it
    uint32_t bad_before = r.gpsdo.badChecksums();

    r.line("GPTXT,01,01,02,SW=V5.1");
    r.line("GPTXT,01,01,01,ANTENNA OK");
    std::vector<uint8_t> tp = {0x40, 0x42, 0x0F, 0x00, 0x64, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0, 0, 0, 0};
    r.feed(cfgFrame(0x03, tp));
    // In pieces
    std::string frame = cfgFrame(0x07, {1, 2, 3, 4, 5});
    r.feed(frame.substr(0, 3));
    r.feed(frame.substr(3));
    r.feed(cfgFrame(0x06, {1, 2, 3, 4}, 1));    // Wrong checksum
    r.feed("\r\n");

    CHECK(r.gpsdo.gpsQuery().running());
    r.advance(GpsQuery::STEP_MS * GpsQuery::stepCount() + GpsQuery::SETTLE_MS + 100);
    CHECK(r.gpsdo.gpsQuery().finished());
    CHECK(!r.gpsdo.gpsQuery().running());

    // Every query was sent, once, and none could change the module's settings
    int gps_queries = 0;
    for (const std::string& s : r.sent) {
        if (s.find("PGPSDO") != std::string::npos) continue;
        gps_queries++;
        if (s[0] == '$') {
            CHECK(s.find("PCAS06,") != std::string::npos);
        } else {
            CHECK_EQ((uint8_t)s[2], 0);
            CHECK_EQ((uint8_t)s[3], 0);
            CHECK_EQ((uint8_t)s[4], 0x06);
        }
    }
    CHECK_EQ(gps_queries, GpsQuery::stepCount());

    const auto& frames = r.gpsdo.gpsQuery().frames();
    CHECK_EQ(frames.size(), 3);
    if (frames.size() == 3) {
        CHECK_EQ(frames[0].id, 0x03);
        CHECK(frames[0].checksum_ok);
        CHECK_STR(frames[0].payload_hex, "40420F00640000000100000000000000");
        CHECK_EQ(frames[1].id, 0x07);
        CHECK(frames[1].checksum_ok);
        CHECK_STR(frames[1].payload_hex, "0102030405");
        CHECK(!frames[2].checksum_ok);
    }
    // The answers, without the sentence repeated every second
    CHECK_EQ(r.gpsdo.gpsQuery().texts().size(), 1);

    // Binary doesn't take the link down, and costs at most what it lands on
    r.tlm(200);
    CHECK_EQ(r.gpsdo.telemetry().uptime, 200);
    CHECK(r.gpsdo.badChecksums() - bad_before <= 3);

    JsonDocument doc;
    CHECK(deserializeJson(doc, gpsQueryJson(r.gpsdo)) == DeserializationError::Ok);
    CHECK_STR(doc["state"].as<std::string>(), "done");
    CHECK_EQ(doc["frames"].size(), 3);
    CHECK_STR(doc["frames"][0]["payload"].as<std::string>(), "40420F00640000000100000000000000");

    // Nothing kept growing once finished
    size_t count = r.gpsdo.gpsQuery().frames().size();
    r.feed(cfgFrame(0x03, tp));
    CHECK_EQ(r.gpsdo.gpsQuery().frames().size(), count);

    // Oversized length is noise, not a frame that swallows the link
    Rig noise;
    noise.ready();
    noise.gpsdo.startGpsQuery(noise.now);
    noise.feed("\xBA\xCE\xFF\xFF");
    noise.feed(cfgFrame(0x03, tp));
    CHECK_EQ(noise.gpsdo.gpsQuery().frames().size(), 1);
}

static void test_console()
{
    static Rig r;
    r.gpsdo.tick(r.now);
    r.tlm();
    std::string text = r.gpsdo.console().text();
    CHECK(text.find("> $PGPSDO,GET,VER*") != std::string::npos);
    CHECK(text.find("$PGPSDO,TLM,100,") != std::string::npos);
    // Keeps 200 lines, newest last, binary made printable
    for (int i = 0; i < 500; i++) r.line("X" + std::to_string(i));
    r.feed(std::string("\x01\xFFz\n"));
    text = r.gpsdo.console().text();
    int lines = 0;
    for (char c : text) if (c == '\n') lines++;
    CHECK_EQ(lines, 200);
    CHECK(text.find("$X301*") != std::string::npos);
    CHECK(text.find("$X300*") == std::string::npos);
    CHECK(text.find("??z\n") != std::string::npos);
}

static std::string slurp(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

static void test_simulator_transcript(const char* clean, const char* corrupted)
{
    {
        static Rig r;
        std::string text = slurp(clean);
        CHECK(text.size() > 10000);
        // Delivered as a UART would: pieces of any size
        for (size_t i = 0; i < text.size(); i += 37) {
            r.gpsdo.onBytes((const uint8_t*)text.data() + i, std::min<size_t>(37, text.size() - i), r.now);
            if (i % 2000 < 37) r.advance(100);
        }
        CHECK_EQ(r.gpsdo.badChecksums(), 0);
        CHECK_EQ(r.gpsdo.telemetry().uptime, 300);
        CHECK_NEAR(r.gpsdo.gps().latitude, -33.8608, 0.001);
        CHECK_EQ(r.gpsdo.gps().sats_used, 9);
        CHECK_EQ(r.gpsdo.gps().sat_count, 11);
        CHECK_STR(r.gpsdo.gps().date, "2023-11-14");
        CHECK_STR(r.gpsdo.firmwareVersion(), "v0.1.16-web1");
        int used = 0;
        for (int i = 0; i < r.gpsdo.gps().sat_count; i++) if (r.gpsdo.gps().sats[i].used) used++;
        CHECK_EQ(used, 9);
    }
    {
        static Rig r;
        std::string text = slurp(corrupted);
        r.gpsdo.onBytes((const uint8_t*)text.data(), text.size(), r.now);
        CHECK(r.gpsdo.badChecksums() > 20);
        CHECK(r.gpsdo.telemetry().valid);
        // Whatever arrived corrupted changed nothing it shouldn't
        CHECK_NEAR(r.gpsdo.gps().latitude, -33.8608, 0.001);
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) {
        std::printf("usage: test_core <transcript> <corrupted transcript>\n");
        return 2;
    }
    struct { const char* name; void (*fn)(); } tests[] = {
        { "protocol document examples", test_examples_from_protocol_document },
        { "bad input", test_bad_input },
        { "link state", test_link_state },
        { "config", test_config },
        { "boot conversation", test_boot_conversation },
        { "commands", test_commands },
        { "GPS", test_gps },
        { "history", test_history },
        { "history from link", test_history_from_link },
        { "reports", test_reports },
        { "console", test_console },
        { "version asked again", test_version_asked_again_when_link_comes_up },
        { "frequency glitches", test_frequency_glitches },
        { "GPS module query", test_gps_query },
    };
    for (auto& t : tests) {
        int before = failures;
        t.fn();
        std::printf("%-32s %s\n", t.name, failures == before ? "ok" : "FAILED");
    }
    int before = failures;
    test_simulator_transcript(argv[1], argv[2]);
    std::printf("%-32s %s\n", "simulator transcript", failures == before ? "ok" : "FAILED");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
