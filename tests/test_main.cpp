#include "test_harness.h"

void test_string_buffer(const char* overlong_text);
void test_time_format();

int main() {
    std::printf("woke.wtf host-side logic tests\n");
    std::printf("==============================\n");

    test_string_buffer("0123456789");
    test_time_format();

    return woke_test::summarize();
}
