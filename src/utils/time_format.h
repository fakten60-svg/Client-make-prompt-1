#pragma once

#include <cstddef>

#include "utils/string_buffer.h"

namespace woke::util {

// Local PC wall-clock time.
//
// The client never formats UTC timestamps: an operator compares a log line against the
// clock on their own machine, so local time is the only useful representation.
struct LocalTime {
    int year = 0;
    int month = 0;       // 1-12
    int day = 0;         // 1-31
    int hour = 0;        // 0-23
    int minute = 0;      // 0-59
    int second = 0;      // 0-59
    int millisecond = 0; // 0-999
};

// Reads the local system clock. Portable: GetLocalTime() on Windows, localtime_r()
// elsewhere, so the formatting logic below stays unit-testable on any host.
LocalTime now_local() noexcept;

// "[YYYY-MM-DD HH:MM:SS.mmm]" - the prefix of every console and file log line.
void format_timestamp(const LocalTime& time, FixedString<40>& out) noexcept;

// "YYYY-MM-DD_HH-MM-SS" - the session log file name required by the logging spec.
void format_session_name(const LocalTime& time, FixedString<32>& out) noexcept;

} // namespace woke::util
