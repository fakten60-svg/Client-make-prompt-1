# Host-side logic tests.
#
# These are deliberately portable: they only touch the parts of the codebase that carry no
# Windows dependency (fixed text buffers, local-time formatting, the mappings registry), so
# they run in seconds on the Linux CI runner as well as on the Windows one, and can be
# executed by hand with any C++20 compiler when no CMake generator is available.

add_executable(woke_tests
    test_main.cpp
    test_string_buffer.cpp
    test_time_format.cpp
    test_mappings_parser.cpp
    test_event_bus.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/jni/mappings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/event_bus.cpp)

target_include_directories(woke_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)

# The parser test validates the anchor table against the asset that actually ships. That is
# what turns "mappings.json is current" from a hope into a build assertion: an upstream
# rename of MinecraftClient.getInstance or of the client's player/world fields fails CI here
# instead of surfacing as a mystery in-game failure.
target_compile_definitions(woke_tests PRIVATE
    WOKE_MAPPINGS_ASSET="${CMAKE_SOURCE_DIR}/mappings.json")

target_link_libraries(woke_tests PRIVATE nlohmann_json)

woke_apply_warnings(woke_tests)

add_test(NAME woke_tests COMMAND woke_tests)
