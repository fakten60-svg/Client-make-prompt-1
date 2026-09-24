#include "test_harness.h"

void test_string_buffer();
void test_time_format();

int main() {
    std::printf("woke.wtf host-side logic tests\n");
    std::printf("==============================\n");

    test_string_buffer();
    test_time_format();

    return woke_test::summarize();
}
