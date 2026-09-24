#include "test_harness.h"

#include "utils/string_buffer.h"

namespace {

using woke::util::FixedString;

} // namespace

// The over-long value arrives as a parameter from another translation unit on purpose.
// A literal here would be a compile-time-provable truncation, which GCC reports as
// -Wformat-truncation - correctly, because in production a literal longer than the
// buffer is usually a bug. Exercising the documented clip path needs a value the
// compiler cannot fold, so the test supplies it across the TU boundary.
void test_string_buffer(const char* overlong_text) {
    woke_test::section("FixedString");

    FixedString<16> text;
    WOKE_CHECK(text.empty());
    WOKE_CHECK(text.size() == 0);
    WOKE_CHECK_STR(text.c_str(), "");

    text.assign("hello");
    WOKE_CHECK(text.size() == 5);
    WOKE_CHECK_STR(text.c_str(), "hello");
    WOKE_CHECK(text.view() == "hello");

    // Capacity 16 stores 15 characters plus the terminator; the rest is clipped.
    text.assign("0123456789abcdefghij");
    WOKE_CHECK(text.size() == 15);
    WOKE_CHECK_STR(text.c_str(), "0123456789abcde");

    text.clear();
    WOKE_CHECK(text.empty());
    WOKE_CHECK_STR(text.c_str(), "");

    // Append fills the remaining room, then clips the overflow.
    text.assign("abc");
    text.append("def");
    WOKE_CHECK_STR(text.c_str(), "abcdef");
    text.append("0123456789abcdefghij");
    WOKE_CHECK(text.size() == 15);
    WOKE_CHECK_STR(text.c_str(), "abcdef012345678");

    FixedString<8> concatenated;
    concatenated += "abc";
    concatenated += "defg";
    WOKE_CHECK_STR(concatenated.c_str(), "abcdefg");

    FixedString<64> formatted;
    formatted.format("%s v%d.%d | %s", "woke.wtf", 0, 1, "x64");
    WOKE_CHECK_STR(formatted.c_str(), "woke.wtf v0.1 | x64");

    FixedString<64> via_helper;
    woke::util::fmt_into(via_helper, "[%04d-%02d-%02d]", 2026, 9, 24);
    WOKE_CHECK_STR(via_helper.c_str(), "[2026-09-24]");

    // An overflowing format must clip cleanly and stay usable afterwards.
    FixedString<8> clipped;
    clipped.format("%s", overlong_text);
    WOKE_CHECK(clipped.size() == 7);
    WOKE_CHECK_STR(clipped.c_str(), "0123456");

    clipped.format("%d", 42);
    WOKE_CHECK(clipped.size() == 2);
    WOKE_CHECK_STR(clipped.c_str(), "42");
}
