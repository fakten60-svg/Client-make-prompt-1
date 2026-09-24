# Host-side logic tests.
#
# These are deliberately portable: they only touch the parts of the codebase that carry
# no Windows dependency (fixed text buffers, local-time formatting), so they run in
# seconds on the Linux CI runner as well as on the Windows one, and can be executed by
# hand with any C++20 compiler when no CMake generator is available.

add_executable(woke_tests
    test_main.cpp
    test_string_buffer.cpp
    test_time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/time_format.cpp)

target_include_directories(woke_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)

woke_apply_warnings(woke_tests)

add_test(NAME woke_tests COMMAND woke_tests)
