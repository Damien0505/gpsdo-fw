#include "gpsdo.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int TLM_FIELDS = 15;
constexpr int MAX_FIELDS = 40;
constexpr uint32_t HISTORY_STEP_MS = 1000;
// Without TLM for this long, a slot of the trend is a gap
constexpr uint32_t HISTORY_GAP_MS = 1500;
constexpr uint32_t CONFIG_RETRY_MS = 5000;
constexpr uint32_t EXPIRE_PERIOD_MS = 1000;
const char PREFIX[] = "PGPSDO,";

int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

uint8_t xorChecksum(const char* begin, const char* end)
{
    uint8_t checksum = 0;
    for (const char* p = begin; p < end; p++) {
        checksum ^= (uint8_t)*p;
    }
    return checksum;
}

bool checksumValid(const char* line)
{
    if (line[0] != '$') {
        return false;
    }
    const char* star = std::strrchr(line, '*');
    if (star == nullptr || star[1] == '\0' || star[2] == '\0' || star[3] != '\0') {
        return false;
    }
    int high = hexDigit(star[1]);
    int low = hexDigit(star[2]);
    return high >= 0 && low >= 0 && xorChecksum(line + 1, star) == (uint8_t)(high << 4 | low);
}

int split(char* text, char** fields, int max)
{
    int n = 0;
    char* p = text;
    while (n < max) {
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

bool parseLong(const char* text, long long* value)
{
    if (*text == '\0') {
        return false;
    }
    char* end;
    *value = std::strtoll(text, &end, 10);
    return *end == '\0';
}

}  // namespace

void ConfigMap::set(const char* key, const char* value)
{
    for (int i = 0; i < count_; i++) {
        if (entries_[i].key == key) {
            entries_[i].value = value;
            return;
        }
    }
    if (count_ < MAX_KEYS) {
        entries_[count_].key = key;
        entries_[count_].value = value;
        count_++;
    }
}

const char* ConfigMap::get(const char* key) const
{
    for (int i = 0; i < count_; i++) {
        if (entries_[i].key == key) {
            return entries_[i].value.c_str();
        }
    }
    return nullptr;
}

void Console::add(const char* prefix, const char* line)
{
    std::string& slot = lines_[written_ % MAX_LINES];
    slot = prefix;
    for (const char* p = line; *p != '\0'; p++) {
        // Binary traffic is possible on this link
        slot += (*p >= 0x20 && *p < 0x7f) ? *p : '?';
    }
    written_++;
}

std::string Console::text() const
{
    std::string out;
    uint32_t count = written_ < MAX_LINES ? written_ : MAX_LINES;
    for (uint32_t i = written_ - count; i != written_; i++) {
        out += lines_[i % MAX_LINES];
        out += '\n';
    }
    return out;
}

bool Gpsdo::linkUp(uint32_t now) const
{
    return tlm_seen_ && (now - last_tlm_) < LINK_TIMEOUT_MS;
}

bool Gpsdo::takeConfigChanged()
{
    bool changed = config_changed_;
    config_changed_ = false;
    return changed;
}

void Gpsdo::onBytes(const uint8_t* data, size_t len, uint32_t now)
{
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n') {
            if (line_overflow_) {
                bad_checksums_++;
            } else {
                if (line_len_ > 0 && line_[line_len_ - 1] == '\r') {
                    line_len_--;
                }
                line_[line_len_] = '\0';
                if (line_len_ > 0) {
                    onLine(line_, now);
                }
            }
            line_len_ = 0;
            line_overflow_ = false;
        } else if (line_len_ >= MAX_LINE) {
            line_overflow_ = true;
        } else if (c == '\0') {
            // Would hide the rest of the line from string functions
            line_overflow_ = true;
        } else {
            line_[line_len_++] = c;
        }
    }
}

void Gpsdo::onLine(const char* line, uint32_t now)
{
    lines_++;
    console_.add("", line);
    if (!checksumValid(line)) {
        bad_checksums_++;
        return;
    }
    if (std::strncmp(line + 1, PREFIX, sizeof(PREFIX) - 1) == 0) {
        char body[MAX_LINE + 1];
        std::strcpy(body, line + 1 + (sizeof(PREFIX) - 1));
        *std::strrchr(body, '*') = '\0';
        onSentence(body, now);
    } else {
        gps_.parse(line, now);
    }
}

void Gpsdo::onSentence(char* body, uint32_t now)
{
    char* fields[MAX_FIELDS];
    int count = split(body, fields, MAX_FIELDS);
    const char* type = fields[0];
    if (std::strcmp(type, "TLM") == 0) {
        onTlm(fields + 1, count - 1, now);
    } else if (std::strcmp(type, "CFG") == 0) {
        onCfg(fields + 1, count - 1);
    } else if (std::strcmp(type, "ACK") == 0) {
        onAck(fields + 1, count - 1, now);
    } else if (std::strcmp(type, "VER") == 0 && count >= 3) {
        protocol_ = std::atoi(fields[1]);
        firmware_ = fields[2];
        // The GPSDO just booted, or answered us: make sure settings are known
        requestConfig(now);
    }
}

void Gpsdo::onTlm(char** f, int count, uint32_t now)
{
    // More fields may be appended by later versions
    if (count < TLM_FIELDS) {
        bad_checksums_++;
        return;
    }
    long long v[TLM_FIELDS];
    for (int i = 0; i < TLM_FIELDS; i++) {
        if (!parseLong(f[i], &v[i])) {
            bad_checksums_++;
            return;
        }
    }
    bool rebooted = tlm_.valid && (uint32_t)v[0] < tlm_.uptime;
    tlm_.valid = true;
    tlm_.uptime = (uint32_t)v[0];
    tlm_.gps_lock = v[1] != 0;
    tlm_.ppb_lock = v[2] != 0;
    tlm_.sats = (int32_t)v[3];
    tlm_.ppb_mean = (int32_t)v[4];
    tlm_.ppb_inst = (int32_t)v[5];
    tlm_.freq = (int32_t)v[6];
    tlm_.error = (int32_t)v[7];
    tlm_.correction = (int32_t)v[8];
    tlm_.pwm = (uint32_t)v[9];
    tlm_.millis = (int32_t)v[10];
    tlm_.pps_shift = (int32_t)v[11];
    tlm_.pps_shift_ms = (int32_t)v[12];
    tlm_.pps_sync_count = (uint32_t)v[13];
    tlm_.adjusting = v[14] != 0;

    tlm_seen_ = true;
    last_tlm_ = now;
    last_history_ = now;
    history_.addValue(tlm_.ppb_mean == PPB_UNKNOWN ? History::NONE : tlm_.ppb_mean);

    if (rebooted) {
        // Settings may have been changed since, or the unit swapped
        config_complete_ = false;
        requestConfig(now);
    }
}

void Gpsdo::onCfg(char** f, int count)
{
    for (int i = 0; i < count; i++) {
        char* equals = std::strchr(f[i], '=');
        if (equals == nullptr) {
            continue;
        }
        *equals = '\0';
        config_.set(f[i], equals + 1);
        // Always last in a report
        if (std::strcmp(f[i], "fw_version") == 0) {
            config_complete_ = true;
            config_changed_ = true;
            firmware_ = equals + 1;
        }
    }
}

void Gpsdo::requestConfig(uint32_t now)
{
    last_config_request_ = now;
    enqueue("GET", "CFG", "", now);
}

CommandResultPtr Gpsdo::enqueue(const std::string& type, const std::string& name, const std::string& rest, uint32_t now)
{
    auto result = std::make_shared<CommandResult>();
    if (!linkUp(now) && !(type == "GET")) {
        result->state = CommandResult::FAILED;
        result->error = "LINK_DOWN";
        return result;
    }
    if (queue_.size() >= MAX_QUEUED_COMMANDS) {
        result->state = CommandResult::FAILED;
        result->error = "BUSY";
        return result;
    }
    std::string body = std::string(PREFIX) + type + "," + name + rest;
    char tail[8];
    std::snprintf(tail, sizeof(tail), "*%02X\r\n", xorChecksum(body.data(), body.data() + body.size()));

    Pending pending;
    pending.type = type;
    pending.name = name;
    pending.sentence = "$" + body + tail;
    pending.result = result;
    queue_.push_back(std::move(pending));
    pump(now);
    return result;
}

CommandResultPtr Gpsdo::set(const char* key, long long value, uint32_t now)
{
    char text[32];
    std::snprintf(text, sizeof(text), ",%lld", value);
    return enqueue("SET", key, text, now);
}

CommandResultPtr Gpsdo::command(const char* name, uint32_t now)
{
    return enqueue("CMD", name, "", now);
}

void Gpsdo::sendCurrent(uint32_t now)
{
    Pending& current = queue_.front();
    current.attempts++;
    current.sent_at = now;
    awaiting_ack_ = true;
    transmit_(current.sentence.data(), current.sentence.size());
    std::string shown = current.sentence.substr(0, current.sentence.size() - 2);
    console_.add("> ", shown.c_str());
}

void Gpsdo::pump(uint32_t now)
{
    if (!awaiting_ack_ && !queue_.empty()) {
        sendCurrent(now);
    }
}

void Gpsdo::finishCurrent(bool ok, const char* error, uint32_t now)
{
    Pending& current = queue_.front();
    current.result->state = ok ? CommandResult::OK : CommandResult::FAILED;
    if (error != nullptr) {
        current.result->error = error;
    }
    queue_.pop_front();
    awaiting_ack_ = false;
    pump(now);
}

void Gpsdo::retryOrFail(const char* error, uint32_t now)
{
    if (queue_.front().attempts < 2) {
        sendCurrent(now);
    } else {
        finishCurrent(false, error, now);
    }
}

void Gpsdo::onAck(char** f, int count, uint32_t now)
{
    if (!awaiting_ack_ || count < 1) {
        return;
    }
    // ACK,ERR,<reason>: the GPSDO could not tell what it received
    if (std::strcmp(f[0], "ERR") == 0) {
        if (count >= 2 && std::strcmp(f[1], "CHECKSUM") == 0) {
            // Damaged on the way: worth sending again
            retryOrFail("CHECKSUM", now);
        } else {
            finishCurrent(false, count >= 2 ? f[1] : "FORMAT", now);
        }
        return;
    }
    const Pending& current = queue_.front();
    if (count < 3 || current.type != f[0]) {
        return;
    }
    // ACK,SET,ERR,FORMAT: no name in the reply when the command had none, which we never send
    if (current.name != f[1]) {
        return;
    }
    if (std::strcmp(f[2], "OK") == 0) {
        finishCurrent(true, nullptr, now);
    } else if (std::strcmp(f[2], "ERR") == 0) {
        finishCurrent(false, count >= 4 ? f[3] : "FORMAT", now);
    }
}

void Gpsdo::tick(uint32_t now)
{
    if (!started_) {
        started_ = true;
        last_history_ = now;
        enqueue("GET", "VER", "", now);
    }

    if (awaiting_ack_ && (now - queue_.front().sent_at) >= ACK_TIMEOUT_MS) {
        retryOrFail("TIMEOUT", now);
    }

    bool up = linkUp(now);
    if (was_up_ && !up) {
        link_downs_++;
    }
    if (up && !was_up_ && !config_complete_) {
        requestConfig(now);
    }
    was_up_ = up;

    // No answer to a config request while the link is up: keep trying
    if (up && !config_complete_ && queue_.empty() && (now - last_config_request_) >= CONFIG_RETRY_MS) {
        requestConfig(now);
    }

    // The trend keeps its time scale during an outage
    while ((now - last_history_) >= HISTORY_GAP_MS) {
        history_.addGap();
        last_history_ += HISTORY_STEP_MS;
    }

    if ((now - last_expire_) >= EXPIRE_PERIOD_MS) {
        last_expire_ = now;
        gps_.expire(now);
    }
}
