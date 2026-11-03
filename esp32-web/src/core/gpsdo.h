// What the ESP32 knows about the GPSDO, and the conversation with it (doc/PROTOCOL.md).
// No Arduino dependency: time is passed in and bytes go out through a callback, so this runs on a PC too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "gps_query.h"
#include "history.h"
#include "nmea.h"

struct Telemetry {
    bool     valid = false;
    uint32_t uptime = 0;
    bool     gps_lock = false;
    bool     ppb_lock = false;
    int32_t  sats = 0;
    int32_t  ppb_mean = 0;      // ppb x 100
    int32_t  ppb_inst = 0;      // ppb x 100
    int32_t  freq = 0;          // Hz
    int32_t  error = 0;         // Hz
    int32_t  correction = 0;    // PWM steps
    uint32_t pwm = 0;
    int32_t  millis = 0;
    int32_t  pps_shift = 0;     // Clock cycles
    int32_t  pps_shift_ms = 0;  // 0.1 us
    uint32_t pps_sync_count = 0;
    bool     adjusting = false;
};

// frequency_get_ppb() value meaning there is no measurement yet
constexpr int32_t PPB_UNKNOWN = 65535;

// Result of a command, shared with whoever is waiting for it
struct CommandResult {
    enum State { PENDING, OK, FAILED } state = PENDING;
    std::string error;   // ACK reason, or TIMEOUT, LINK_DOWN, BUSY
};
using CommandResultPtr = std::shared_ptr<CommandResult>;

// Settings as last reported by CFG. Values stay as text: they are only ever integers or fw_version.
class ConfigMap {
public:
    static constexpr int MAX_KEYS = 32;
    struct Entry { std::string key, value; };

    void set(const char* key, const char* value);
    const char* get(const char* key) const;
    int size() const { return count_; }
    const Entry& at(int i) const { return entries_[i]; }

private:
    Entry entries_[MAX_KEYS];
    int   count_ = 0;
};

class Console {
public:
    static constexpr int MAX_LINES = 200;
    void add(const char* prefix, const char* line);
    std::string text() const;

private:
    std::string lines_[MAX_LINES];
    uint32_t    written_ = 0;
};

class Gpsdo {
public:
    using Transmit = std::function<void(const char* data, size_t len)>;

    static constexpr uint32_t LINK_TIMEOUT_MS = 5000;
    static constexpr uint32_t ACK_TIMEOUT_MS = 2000;
    static constexpr size_t   MAX_QUEUED_COMMANDS = 16;
    static constexpr size_t   MAX_LINE = 200;

    explicit Gpsdo(Transmit transmit) : transmit_(std::move(transmit)) {}

    void onBytes(const uint8_t* data, size_t len, uint32_t now);
    // Call often: timeouts, retries, history
    void tick(uint32_t now);

    // key is sent as is: the caller validates it (lowercase, digits and underscore only)
    CommandResultPtr set(const char* key, long long value, uint32_t now);
    // SAVEPWM, FORCESYNC
    CommandResultPtr command(const char* name, uint32_t now);

    bool linkUp(uint32_t now) const;
    uint32_t linkAgeMs(uint32_t now) const { return tlm_seen_ ? now - last_tlm_ : UINT32_MAX; }

    const Telemetry& telemetry() const { return tlm_; }
    const GpsData& gps() const { return gps_; }
    const ConfigMap& config() const { return config_; }
    bool configComplete() const { return config_complete_; }
    const History& history() const { return history_; }
    const Console& console() const { return console_; }
    const std::string& firmwareVersion() const { return firmware_; }
    int protocolVersion() const { return protocol_; }

    uint32_t badChecksums() const { return bad_checksums_; }
    uint32_t linesReceived() const { return lines_; }
    uint32_t linkDownEvents() const { return link_downs_; }
    uint32_t measurementGlitches() const { return glitches_; }

    // Reads the GPS module's settings, see gps_query.h
    void startGpsQuery(uint32_t now) { gps_query_.start(now); }
    const GpsQuery& gpsQuery() const { return gps_query_; }

    // True once when a complete CFG report arrived since the last call
    bool takeConfigChanged();

private:
    struct Pending {
        std::string type, name, sentence;
        CommandResultPtr result;
        int attempts = 0;
        uint32_t sent_at = 0;
    };

    void onLine(const char* line, uint32_t now);
    void onSentence(char* body, uint32_t now);
    void onTlm(char** fields, int count, uint32_t now);
    void onCfg(char** fields, int count);
    void onAck(char** fields, int count, uint32_t now);
    void requestConfig(uint32_t now);
    CommandResultPtr enqueue(const std::string& type, const std::string& name, const std::string& rest, uint32_t now);
    void pump(uint32_t now);
    void sendCurrent(uint32_t now);
    void finishCurrent(bool ok, const char* error, uint32_t now);
    void retryOrFail(const char* error, uint32_t now);

    Transmit  transmit_;
    Telemetry tlm_;
    GpsData   gps_;
    ConfigMap config_;
    History   history_;
    Console   console_;
    GpsQuery  gps_query_;

    std::string firmware_;
    int       protocol_ = 0;
    bool      config_complete_ = false;
    bool      config_changed_ = false;

    char      line_[MAX_LINE + 1];
    size_t    line_len_ = 0;
    bool      line_overflow_ = false;

    uint32_t  bad_checksums_ = 0;
    uint32_t  lines_ = 0;
    uint32_t  link_downs_ = 0;
    uint32_t  glitches_ = 0;
    int       glitch_run_ = 0;

    bool      tlm_seen_ = false;
    uint32_t  last_tlm_ = 0;
    bool      was_up_ = false;
    bool      started_ = false;
    uint32_t  last_history_ = 0;
    uint32_t  last_config_request_ = 0;
    uint32_t  last_expire_ = 0;

    std::deque<Pending> queue_;
    bool      awaiting_ack_ = false;
};
