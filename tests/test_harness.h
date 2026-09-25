#pragma once

// Dependency-free test harness.
//
// The client ships as a single self-contained DLL, so the test target is kept equally
// self-contained: no test framework, no extra dependency to vendor or pin. It reports
// every failure it can and exits non-zero, which is all CTest needs.

#include <cstdio>
#include <cstring>

namespace woke_test {

inline int g_checks = 0;
inline int g_failures = 0;

inline void section(const char* name) {
    // Flushed per section, not just at exit: if a section ever crashes on one host only
    // (different CRT, different optimiser), CI must say which section died instead of
    // reporting a silent segfault with no prior output.
    std::printf("\n[%s]\n", name);
    std::fflush(stdout);
}

inline void report(const char* file, int line, const char* expression) {
    ++g_failures;
    std::printf("  FAIL %s:%d: %s\n", file, line, expression);
}

inline void check_string(const char* file, int line, const char* expression, const char* actual,
    const char* expected) {
    ++g_checks;
    const bool equal = (actual != nullptr && expected != nullptr)
                           ? std::strcmp(actual, expected) == 0
                           : (actual == expected);
    if (equal) {
        return;
    }
    report(file, line, expression);
    std::printf("       actual:   \"%s\"\n", actual != nullptr ? actual : "(null)");
    std::printf("       expected: \"%s\"\n", expected != nullptr ? expected : "(null)");
    std::fflush(stdout);
}

inline int summarize() {
    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

} // namespace woke_test

#define WOKE_CHECK(condition)                                     \
    do {                                                          \
        ++woke_test::g_checks;                                    \
        if (!(condition)) {                                       \
            woke_test::report(__FILE__, __LINE__, #condition);    \
            std::fflush(stdout);                                \
        }                                                         \
    } while (false)

#define WOKE_CHECK_STR(actual, expected) \
    woke_test::check_string(__FILE__, __LINE__, #actual " == " #expected, (actual), (expected))
