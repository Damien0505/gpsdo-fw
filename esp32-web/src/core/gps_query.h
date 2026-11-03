// Reads the settings of the GPS module, on request. Only ever sends a fixed list of queries, which change nothing.
// The GPSDO forwards bytes it doesn't recognise to the GPS, and everything from the GPS back to here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class GpsQuery {
public:
    using Transmit = std::function<void(const char* data, size_t len)>;

    static constexpr uint32_t STEP_MS = 1500;
    // Replies wait for the module's next gap in its output
    static constexpr uint32_t SETTLE_MS = 3000;
    static constexpr size_t   MAX_PAYLOAD = 64;
    static constexpr size_t   MAX_FRAMES = 16;
    static constexpr size_t   MAX_TEXTS = 8;

    struct Frame {
        uint8_t     cls = 0;
        uint8_t     id = 0;
        std::string payload_hex;
        bool        checksum_ok = false;
    };

    // Forgets the previous results. Does nothing if one is running.
    void start(uint32_t now);
    void tick(uint32_t now, const Transmit& transmit);

    bool running() const { return running_; }
    bool finished() const { return finished_; }
    int  stepsSent() const { return step_; }
    static int stepCount();

    // All bytes from the link, while running: binary replies are found here
    void onByte(uint8_t byte);
    // Valid text sentences from the GPS, while running
    void onText(const char* line);

    const std::vector<Frame>&       frames() const { return frames_; }
    const std::vector<std::string>& texts() const { return texts_; }

    // Exposed for the tests
    static std::string textQuery(const char* body);
    static std::string binaryQuery(uint8_t cls, uint8_t id);

private:
    enum State { WAIT_B, WAIT_C, LEN_LOW, LEN_HIGH, BODY };

    void finishFrame();

    bool     running_ = false;
    bool     finished_ = false;
    int      step_ = 0;
    uint32_t last_step_ = 0;

    State    state_ = WAIT_B;
    size_t   length_ = 0;
    size_t   received_ = 0;
    uint8_t  body_[2 + MAX_PAYLOAD + 4];

    std::vector<Frame>       frames_;
    std::vector<std::string> texts_;
};
