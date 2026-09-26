#!/usr/bin/env bash
# Build + run the portable host-test suite without CMake.
#
# CI (and any normal machine) uses CMake; this script replicates the `woke_tests`
# target by hand for sandbox hosts that ship a C++20 toolchain but no cmake
# binary. It mirrors tests/CMakeLists.txt exactly:
#   - the same translation-unit list,
#   - the same include paths (src, tests, ImGui, nlohmann/json),
#   - the same compile definitions, with WOKE_MAPPINGS_ASSET / WOKE_MODULES_SOURCE_DIR
#     pointed at this checkout (the embedded quotes are part of the macro value),
#   - ImGui's four core objects compiled into the SAME object directory as the
#     first-party TUs - the test target links ImGui's core statically, and an
#     ad-hoc build that forgets them fails at link time with unresolved
#     ImGui:: symbols.
#
# Dependencies (pinned tags, matching cmake/ThirdParty.cmake):
#   git clone --depth 1 --branch v1.92.9b-docking https://github.com/ocornut/imgui.git /tmp/woke-deps/imgui
#   git clone --depth 1 --branch v3.12.0           https://github.com/nlohmann/json.git /tmp/woke-deps/nlohmann
# Override either location with WOKE_IMGUI_DIR / WOKE_JSON_DIR, and the compiler
# with CXX (default g++-11).

set -euo pipefail

cd "$(dirname "$0")/.."

CXX_BIN="${CXX:-g++-11}"
IMGUI_DIR="${WOKE_IMGUI_DIR:-/tmp/woke-deps/imgui}"
JSON_DIR="${WOKE_JSON_DIR:-/tmp/woke-deps/nlohmann/include}"
OUT="${1:-build/objs-host}"

if [[ ! -d "$IMGUI_DIR" ]]; then
    echo "error: ImGui checkout not found at $IMGUI_DIR (set WOKE_IMGUI_DIR)" >&2
    exit 1
fi
if [[ ! -d "$JSON_DIR" ]]; then
    echo "error: nlohmann/json checkout not found at $JSON_DIR (set WOKE_JSON_DIR)" >&2
    exit 1
fi

WARN="-std=c++20 -Wall -Wextra -Wpedantic -Werror"

# tests/CMakeLists.txt's add_executable() list, in the same order.
TEST_SOURCES=(
    tests/test_main.cpp
    tests/test_string_buffer.cpp
    tests/test_time_format.cpp
    tests/test_mappings_parser.cpp
    tests/test_event_bus.cpp
    tests/test_ui_animation.cpp
    tests/test_ui_draw.cpp
    tests/test_module_system.cpp
    tests/test_widgets.cpp
    tests/test_visual_modules.cpp
    tests/test_step8_modules.cpp
    tests/test_combat_mace.cpp
    tests/test_spear_misc.cpp
    tests/test_perf.cpp
)
# The first-party TUs the test target links (portable code only).
CORE_SOURCES=(
    src/utils/time_format.cpp
    src/utils/render_utils.cpp
    src/jni/mappings.cpp
    src/core/event_bus.cpp
    src/core/config.cpp
    src/modules/module_manager.cpp
    src/ui/theme.cpp
    src/ui/animation/animation_controller.cpp
    src/ui/components/traffic_lights.cpp
    src/ui/components/pill_toggle.cpp
    src/ui/components/keybind_badge.cpp
    src/ui/components/module_card.cpp
    src/ui/components/sidebar.cpp
    src/ui/components/search_bar.cpp
    src/ui/notifications.cpp
    src/modules/game_writes.cpp
    src/modules/client_sound.cpp
    src/ui/hud.cpp
    src/ui/overlay.cpp
)
# The ImGui core target from cmake/ThirdParty.cmake.
IMGUI_SOURCES=(
    "$IMGUI_DIR/imgui.cpp"
    "$IMGUI_DIR/imgui_draw.cpp"
    "$IMGUI_DIR/imgui_tables.cpp"
    "$IMGUI_DIR/imgui_widgets.cpp"
)

mkdir -p "$OUT"

FLAGS=(-Isrc -Itests -I"$IMGUI_DIR" -I"$JSON_DIR"
    "-DWOKE_MAPPINGS_ASSET=\"$PWD/mappings.json\""
    "-DWOKE_MODULES_SOURCE_DIR=\"$PWD/src/modules\"")

for src in "${TEST_SOURCES[@]}" "${CORE_SOURCES[@]}" "${IMGUI_SOURCES[@]}"; do
    obj="$OUT/$(echo "$src" | tr '/' '_').o"
    if [[ -f "$obj" && ! "$src" -nt "$obj" ]]; then
        continue
    fi
    echo "CXX $src"
    "$CXX_BIN" $WARN "${FLAGS[@]}" -c "$src" -o "$obj"
done

echo "LINK $OUT/woke_tests"
"$CXX_BIN" -o "$OUT/woke_tests" "$OUT"/*.o

echo "RUN $OUT/woke_tests"
"$OUT/woke_tests"
