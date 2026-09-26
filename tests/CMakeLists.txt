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
    test_ui_animation.cpp
    test_ui_draw.cpp
    test_module_system.cpp
    test_widgets.cpp
    test_visual_modules.cpp
    test_step8_modules.cpp                                    # Step 8: bind persistence, dispatch, action modules
    test_combat_mace.cpp                                      # Step 8b: the Combat/Mace readouts
    test_spear_misc.cpp                                       # Step 8c: the Misc/Movement/Spear readouts
    test_perf.cpp                                             # Step 9: budgets, EMA, §11 gate
    ${CMAKE_SOURCE_DIR}/src/utils/time_format.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/render_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/jni/mappings.cpp
    ${CMAKE_SOURCE_DIR}/src/core/event_bus.cpp
    ${CMAKE_SOURCE_DIR}/src/core/config.cpp
    ${CMAKE_SOURCE_DIR}/src/modules/module_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/animation/animation_controller.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/components/traffic_lights.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/components/pill_toggle.cpp
    ${CMAKE_SOURCE_DIR}/src/ui/components/keybind_badge.cpp       # Step 6: the [KEY: X] chip
    ${CMAKE_SOURCE_DIR}/src/ui/components/module_card.cpp         # Step 6: the module card
    ${CMAKE_SOURCE_DIR}/src/ui/components/sidebar.cpp             # Step 6: navigation + badges
    ${CMAKE_SOURCE_DIR}/src/ui/components/search_bar.cpp          # Step 6: the module filter
    ${CMAKE_SOURCE_DIR}/src/ui/notifications.cpp                  # Step 6: the toast pool
    ${CMAKE_SOURCE_DIR}/src/modules/game_writes.cpp               # Step 7: the write seam
    ${CMAKE_SOURCE_DIR}/src/modules/client_sound.cpp              # Step 8c: the Client Sound sink (portable)
    ${CMAKE_SOURCE_DIR}/src/ui/hud.cpp                            # Step 7: the overlay renderer
    ${CMAKE_SOURCE_DIR}/src/ui/overlay.cpp)                        # Step 7: the overlay policy

target_include_directories(woke_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests)

# The parser test validates the anchor table against the asset that actually ships. That is
# what turns "mappings.json is current" from a hope into a build assertion: an upstream
# rename of MinecraftClient.getInstance or of the client's player/world fields fails CI here
# instead of surfacing as a mystery in-game failure. The step-9 network-safety scan walks the
# module tree the same way, so it gets its own path define.
target_compile_definitions(woke_tests PRIVATE
    WOKE_MAPPINGS_ASSET="${CMAKE_SOURCE_DIR}/mappings.json"
    WOKE_MODULES_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/modules")

# ImGui's core is host-independent, so the draw helpers and the components are rendered headlessly
# here: the test asserts the draw data ImVec2/ImDrawList code produces, which is the closest a
# Linux runner can get to "the chrome draws something sane". The platform backends are not needed
# and are not linked.
target_link_libraries(woke_tests PRIVATE nlohmann_json imgui)

woke_apply_warnings(woke_tests)

add_test(NAME woke_tests COMMAND woke_tests)
