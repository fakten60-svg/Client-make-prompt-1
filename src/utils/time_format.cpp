#include "utils/time_format.h"

#include <ctime>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/time.h>
#endif

namespace woke::util {
namespace {

// Defensive clamping: a malformed clock (or a caller-built struct in tests) must never
// produce a field like "%013" in a log line or an unparseable file name.
int clamp_value(int value, int low, int high) noexcept {
    if (value < low) {
        return low;
    }
    return value > high ? high : value;
}

} // namespace

LocalTime now_local() noexcept {
    LocalTime result{};

#if defined(_WIN32)
    SYSTEMTIME local{};
    ::GetLocalTime(&local);
    result.year = static_cast<int>(local.wYear);
    result.month = static_cast<int>(local.wMonth);
    result.day = static_cast<int>(local.wDay);
    result.hour = static_cast<int>(local.wHour);
    result.minute = static_cast<int>(local.wMinute);
    result.second = static_cast<int>(local.wSecond);
    result.millisecond = static_cast<int>(local.wMilliseconds);
#else
    timeval tv{};
    ::gettimeofday(&tv, nullptr);
    const std::time_t seconds = static_cast<std::time_t>(tv.tv_sec);
    std::tm local{};
    if (::localtime_r(&seconds, &local) != nullptr) {
        result.year = local.tm_year + 1900;
        result.month = local.tm_mon + 1;
        result.day = local.tm_mday;
        result.hour = local.tm_hour;
        result.minute = local.tm_min;
        result.second = local.tm_sec;
    }
    result.millisecond = static_cast<int>(tv.tv_usec / 1000);
#endif

    return result;
}

void format_timestamp(const LocalTime& time, FixedString<40>& out) noexcept {
    out.format("[%04d-%02d-%02d %02d:%02d:%02d.%03d]",
        clamp_value(time.year, 0, 9999),
        clamp_value(time.month, 1, 12),
        clamp_value(time.day, 1, 31),
        clamp_value(time.hour, 0, 23),
        clamp_value(time.minute, 0, 59),
        clamp_value(time.second, 0, 59),
        clamp_value(time.millisecond, 0, 999));
}

void format_session_name(const LocalTime& time, FixedString<32>& out) noexcept {
    out.format("%04d-%02d-%02d_%02d-%02d-%02d",
        clamp_value(time.year, 0, 9999),
        clamp_value(time.month, 1, 12),
        clamp_value(time.day, 1, 31),
        clamp_value(time.hour, 0, 23),
        clamp_value(time.minute, 0, 59),
        clamp_value(time.second, 0, 59));
}

} // namespace woke::util
