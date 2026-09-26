#include "test_harness.h"

void test_string_buffer(const char* overlong_text);
void test_time_format();
void test_mappings_parser();
void test_event_bus();
void test_ui_animation();
void test_ui_draw();
void test_module_system();
void test_widgets();
void test_visual_modules();
void test_step8_modules();
void test_combat_mace();

int main() {
    std::printf("woke.wtf host-side logic tests\n");
    std::printf("==============================\n");

    test_string_buffer("0123456789");
    test_time_format();
    test_mappings_parser();
    test_event_bus();
    test_ui_animation();
    test_ui_draw();
    test_module_system();
    test_widgets();
    test_visual_modules();
    test_step8_modules();
    test_combat_mace();

    return woke_test::summarize();
}
