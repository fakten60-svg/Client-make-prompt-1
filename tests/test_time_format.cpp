#include "test_harness.h"

#include "utils/time_format.h"

namespace {

using woke::util::FixedString;
using woke::util::LocalTime;

} // namespace

void test_time_format() {
    woke_test::section("local time formatting");

    LocalTime sample{};
    sample.year = 2026;
    sample.month = 9;
    sample.day = 20;
    sample.hour = 15;
    sample.minute = 41;
    sample.second = 0;
    sample.millisecond = 0;

    // The exact shape mandated by the logging spec.
    FixedString<40> stamp;
    woke::util::format_timestamp(sample, stamp);
    WOKE_CHECK_STR(stamp.c_str(), "[2026-09-20 15:41:00.000]");

    FixedString<32> session;
    woke::util::format_session_name(sample, session);
    WOKE_CHECK_STR(session.c_str(), "2026-09-20_15-41-00");

    // Zero padding on every single-digit field.
    LocalTime padded{};
    padded.year = 2026;
    padded.month = 1;
    padded.day = 4;
    padded.hour = 3;
    padded.minute = 7;
    padded.second = 9;
    padded.millisecond = 7;
    woke::util::format_timestamp(padded, stamp);
    WOKE_CHECK_STR(stamp.c_str(), "[2026-01-04 03:07:09.007]");

    woke::util::format_session_name(padded, session);
    WOKE_CHECK_STR(session.c_str(), "2026-01-04_03-07-09");

    // Impossible values are clamped instead of being rendered as garbage.
    LocalTime hostile{};
    hostile.year = 20261;
    hostile.month = 44;
    hostile.day = 0;
    hostile.hour = 99;
    hostile.minute = -3;
    hostile.second = 61;
    hostile.millisecond = 5000;
    woke::util::format_timestamp(hostile, stamp);
    WOKE_CHECK_STR(stamp.c_str(), "[9999-12-01 23:00:59.999]");

    // The live clock must land inside sane ranges on any machine.
    const LocalTime now = woke::util::now_local();
    WOKE_CHECK(now.year >= 2024 && now.year <= 2200);
    WOKE_CHECK(now.month >= 1 && now.month <= 12);
    WOKE_CHECK(now.day >= 1 && now.day <= 31);
    WOKE_CHECK(now.hour >= 0 && now.hour <= 23);
    WOKE_CHECK(now.minute >= 0 && now.minute <= 59);
    WOKE_CHECK(now.second >= 0 && now.second <= 59);
    WOKE_CHECK(now.millisecond >= 0 && now.millisecond <= 999);

    FixedString<40> live;
    woke::util::format_timestamp(now, live);
    WOKE_CHECK(live.size() == 25);  // "[YYYY-MM-DD HH:MM:SS.mmm]" is always 25 characters
    WOKE_CHECK(live.c_str()[0] == '[');
    WOKE_CHECK(live.c_str()[24] == ']');
}
