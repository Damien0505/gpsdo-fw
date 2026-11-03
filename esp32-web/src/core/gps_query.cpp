#include "gps_query.h"

#include <cstdio>
#include <cstring>

namespace {

// See the CASIC protocol specification
constexpr uint8_t SYNC_1 = 0xBA;
constexpr uint8_t SYNC_2 = 0xCE;
constexpr uint8_t CLASS_CFG = 0x06;

struct Step {
    const char* text;   // A sentence if not null, otherwise an empty CFG message, which asks for the setting
    uint8_t     id;
};

const Step STEPS[] = {
    { "PCAS06,0", 0 },         // Firmware version
    { "PCAS06,1", 0 },         // Hardware model and serial number
    { "PCAS06,2", 0 },         // Working mode
    { nullptr, 0x03 },         // CFG-TP: time pulse
    { nullptr, 0x04 },         // CFG-RATE: update rate
    { nullptr, 0x06 },         // CFG-TMODE: timing mode
    { nullptr, 0x07 },         // CFG-NAVX: navigation engine
};

uint32_t readWord(const uint8_t* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Word by word sum, starting from the message class, id and length
uint32_t checksum(uint8_t cls, uint8_t id, size_t length, const uint8_t* payload)
{
    uint32_t sum = ((uint32_t)cls << 24) + ((uint32_t)id << 16) + (uint32_t)length;
    for (size_t i = 0; i + 4 <= length; i += 4) {
        sum += readWord(payload + i);
    }
    return sum;
}

}  // namespace

int GpsQuery::stepCount()
{
    return (int)(sizeof(STEPS) / sizeof(STEPS[0]));
}

std::string GpsQuery::textQuery(const char* body)
{
    uint8_t sum = 0;
    for (const char* p = body; *p != '\0'; p++) {
        sum ^= (uint8_t)*p;
    }
    char tail[8];
    std::snprintf(tail, sizeof(tail), "*%02X\r\n", sum);
    return std::string("$") + body + tail;
}

std::string GpsQuery::binaryQuery(uint8_t cls, uint8_t id)
{
    uint32_t sum = checksum(cls, id, 0, nullptr);
    std::string out;
    out += (char)SYNC_1;
    out += (char)SYNC_2;
    out += '\0';    // Length
    out += '\0';
    out += (char)cls;
    out += (char)id;
    for (int shift = 0; shift < 32; shift += 8) {
        out += (char)(sum >> shift);
    }
    // Not part of the message: ends the GPSDO's passthrough, so that its next command isn't sent to the GPS.
    // The module has to ignore bytes between messages anyway.
    out += "\r\n";
    return out;
}

void GpsQuery::start(uint32_t now)
{
    if (running_) {
        return;
    }
    running_ = true;
    finished_ = false;
    step_ = 0;
    // The first query goes out on the next tick
    last_step_ = now - STEP_MS;
    state_ = WAIT_B;
    frames_.clear();
    texts_.clear();
}

void GpsQuery::tick(uint32_t now, const Transmit& transmit)
{
    if (!running_) {
        return;
    }
    if (step_ < stepCount()) {
        if ((now - last_step_) >= STEP_MS) {
            last_step_ = now;
            const Step& step = STEPS[step_++];
            std::string query = step.text != nullptr ? textQuery(step.text) : binaryQuery(CLASS_CFG, step.id);
            transmit(query.data(), query.size());
        }
    } else if ((now - last_step_) >= SETTLE_MS) {
        running_ = false;
        finished_ = true;
    }
}

void GpsQuery::onByte(uint8_t byte)
{
    switch (state_) {
        case WAIT_B:
            if (byte == SYNC_1) {
                state_ = WAIT_C;
            }
            break;
        case WAIT_C:
            state_ = byte == SYNC_2 ? LEN_LOW : (byte == SYNC_1 ? WAIT_C : WAIT_B);
            break;
        case LEN_LOW:
            length_ = byte;
            state_ = LEN_HIGH;
            break;
        case LEN_HIGH:
            length_ |= (size_t)byte << 8;
            received_ = 0;
            // Noise that looks like a start, or more than any setting
            state_ = length_ <= MAX_PAYLOAD ? BODY : WAIT_B;
            break;
        case BODY:
            body_[received_++] = byte;
            if (received_ == 2 + length_ + 4) {
                finishFrame();
                state_ = WAIT_B;
            }
            break;
    }
}

void GpsQuery::finishFrame()
{
    if (frames_.size() >= MAX_FRAMES) {
        return;
    }
    Frame frame;
    frame.cls = body_[0];
    frame.id = body_[1];
    const uint8_t* payload = body_ + 2;
    frame.checksum_ok = checksum(frame.cls, frame.id, length_, payload) == readWord(payload + length_);
    char hex[3];
    for (size_t i = 0; i < length_; i++) {
        std::snprintf(hex, sizeof(hex), "%02X", payload[i]);
        frame.payload_hex += hex;
    }
    frames_.push_back(frame);
}

void GpsQuery::onText(const char* line)
{
    // Only the answers: the module sends this one every second
    if (std::strstr(line, "TXT") == nullptr || std::strstr(line, "ANTENNA") != nullptr) {
        return;
    }
    if (texts_.size() < MAX_TEXTS) {
        texts_.emplace_back(line);
    }
}
