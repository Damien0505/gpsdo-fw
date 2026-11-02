// PPB trend in RAM: 1 s resolution for the last hour, 1 minute min/mean/max for the last 24 h.
// Lost when the ESP32 reboots. No Arduino dependency.
#pragma once

#include <cstddef>
#include <cstdint>

class History {
public:
    static constexpr int FINE_SIZE = 3600;
    static constexpr int COARSE_SIZE = 1440;
    static constexpr int SAMPLES_PER_MINUTE = 60;
    // A gap: no link, or no measurement yet
    static constexpr int32_t NONE = INT32_MIN;

    void addValue(int32_t ppb_x100);
    void addGap() { addValue(NONE); }

    enum Range { RANGE_1H, RANGE_24H };

    // Streams a JSON document in pieces, as the web server asks for them
    struct Reader {
        Range    range;
        uint32_t total = 0;      // Entries to send
        uint32_t position = 0;   // Entries sent
        uint32_t first = 0;      // Sequence number of the oldest entry
        int      stage = 0;
    };
    Reader startReader(Range range) const;
    // Fills buffer, returns the length. 0 means finished.
    size_t read(Reader& reader, char* buffer, size_t max) const;

    uint32_t fineCount() const { return fine_written_ < FINE_SIZE ? fine_written_ : FINE_SIZE; }
    uint32_t coarseCount() const { return coarse_written_ < COARSE_SIZE ? coarse_written_ : COARSE_SIZE; }

private:
    struct Minute { int32_t min, mean, max; };

    int32_t  fine_[FINE_SIZE];
    uint32_t fine_written_ = 0;
    Minute   coarse_[COARSE_SIZE];
    uint32_t coarse_written_ = 0;

    int      minute_samples_ = 0;
    int      minute_values_ = 0;
    int64_t  minute_sum_ = 0;
    int32_t  minute_min_ = 0;
    int32_t  minute_max_ = 0;

    size_t formatEntry(const Reader& reader, char* buffer, size_t max) const;
};
