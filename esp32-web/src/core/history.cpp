#include "history.h"

#include <cstdio>
#include <cstring>

void History::addValue(int32_t value)
{
    fine_[fine_written_ % FINE_SIZE] = value;
    fine_written_++;

    if (value != NONE) {
        if (minute_values_ == 0 || value < minute_min_) minute_min_ = value;
        if (minute_values_ == 0 || value > minute_max_) minute_max_ = value;
        minute_sum_ += value;
        minute_values_++;
    }
    if (++minute_samples_ >= SAMPLES_PER_MINUTE) {
        Minute& m = coarse_[coarse_written_ % COARSE_SIZE];
        if (minute_values_ > 0) {
            m.min = minute_min_;
            m.mean = (int32_t)(minute_sum_ / minute_values_);
            m.max = minute_max_;
        } else {
            m.min = m.mean = m.max = NONE;
        }
        coarse_written_++;
        minute_samples_ = 0;
        minute_values_ = 0;
        minute_sum_ = 0;
    }
}

History::Reader History::startReader(Range range) const
{
    Reader reader;
    reader.range = range;
    if (range == RANGE_1H) {
        reader.total = fineCount();
        reader.first = fine_written_ - reader.total;
    } else {
        reader.total = coarseCount();
        reader.first = coarse_written_ - reader.total;
    }
    return reader;
}

static size_t formatValue(int32_t value, char* buffer, size_t max)
{
    int n = value == History::NONE ? std::snprintf(buffer, max, "null")
                                   : std::snprintf(buffer, max, "%ld", (long)value);
    return n > 0 ? (size_t)n : 0;
}

size_t History::formatEntry(const Reader& reader, char* buffer, size_t max) const
{
    uint32_t sequence = reader.first + reader.position;
    size_t len = 0;
    if (reader.position > 0) {
        buffer[len++] = ',';
    }
    if (reader.range == RANGE_1H) {
        len += formatValue(fine_[sequence % FINE_SIZE], buffer + len, max - len);
    } else {
        const Minute& m = coarse_[sequence % COARSE_SIZE];
        buffer[len++] = '[';
        len += formatValue(m.min, buffer + len, max - len);
        buffer[len++] = ',';
        len += formatValue(m.mean, buffer + len, max - len);
        buffer[len++] = ',';
        len += formatValue(m.max, buffer + len, max - len);
        buffer[len++] = ']';
    }
    return len;
}

size_t History::read(Reader& reader, char* buffer, size_t max) const
{
    // Room for one entry at least, three 11 digit values and separators
    constexpr size_t ENTRY_MAX = 44;
    size_t len = 0;
    if (reader.stage == 0) {
        if (max < 64) {
            return 0;
        }
        len += std::snprintf(buffer, max, "{\"range\":\"%s\",\"step_s\":%d,\"count\":%lu,\"rows\":[",
                             reader.range == RANGE_1H ? "1h" : "24h",
                             reader.range == RANGE_1H ? 1 : SAMPLES_PER_MINUTE, (unsigned long)reader.total);
        reader.stage = 1;
    }
    while (reader.stage == 1 && reader.position < reader.total && (max - len) > ENTRY_MAX) {
        len += formatEntry(reader, buffer + len, max - len);
        reader.position++;
    }
    if (reader.stage == 1 && reader.position >= reader.total && (max - len) >= 3) {
        buffer[len++] = ']';
        buffer[len++] = '}';
        reader.stage = 2;
    }
    return len;
}
