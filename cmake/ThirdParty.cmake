# Vendored dependency acquisition.
#
# Every dependency is pinned to a verified upstream tag. We deliberately do NOT run
# the upstream CMakeLists.txt files: we declare the targets ourselves so that what we
# compile can never silently change when upstream reorganises its build. The
# SOURCE_SUBDIR is pointed at a path that does not exist, which makes
# FetchContent_MakeAvailable populate the sources without calling add_subdirectory().
#
# Verified tags (checked against the upstream repositories):
#   ocornut/imgui        v1.92.9b-docking   (docking line, per decision D-06)
#   TsudaKageyu/minhook  v1.3.4
#   nlohmann/json        v3.12.0

include(FetchContent)

set(WOKE_IMGUI_TAG   "v1.92.9b-docking" CACHE STRING "Dear ImGui tag or commit to build against")
set(WOKE_MINHOOK_TAG "v1.3.4"           CACHE STRING "MinHook tag or commit to build against")
set(WOKE_JSON_TAG    "v3.12.0"          CACHE STRING "nlohmann/json tag or commit to build against")

# nlohmann/json is fetched on every host: the mapping registry is portable, so the host
# tests parse mappings.json (and every schema variant) on Linux as well as on Windows.
FetchContent_Declare(woke_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        ${WOKE_JSON_TAG}
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  woke-does-not-build-upstream-cmakelists)

FetchContent_MakeAvailable(woke_json)

# ── nlohmann/json (header only) ──
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${woke_json_SOURCE_DIR}/include)

# ImGui and MinHook are only needed by the injected artifact, so they are skipped entirely
# on a non-Windows host instead of being cloned for nothing.
if(NOT WIN32)
    return()
endif()

FetchContent_Declare(woke_imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${WOKE_IMGUI_TAG}
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  woke-does-not-build-upstream-cmakelists)

FetchContent_Declare(woke_minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        ${WOKE_MINHOOK_TAG}
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  woke-does-not-build-upstream-cmakelists)

FetchContent_MakeAvailable(woke_imgui woke_minhook)

# ── Dear ImGui (Win32 + OpenGL3 backends only — the renderer path Minecraft uses) ──
add_library(imgui STATIC
    ${woke_imgui_SOURCE_DIR}/imgui.cpp
    ${woke_imgui_SOURCE_DIR}/imgui_draw.cpp
    ${woke_imgui_SOURCE_DIR}/imgui_tables.cpp
    ${woke_imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${woke_imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${woke_imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${woke_imgui_SOURCE_DIR}
    ${woke_imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    WIN32_LEAN_AND_MEAN
    NOMINMAX)

# ── MinHook (x64 trampoline engine) ──
add_library(minhook STATIC
    ${woke_minhook_SOURCE_DIR}/src/hde/hde64.c
    ${woke_minhook_SOURCE_DIR}/src/buffer.c
    ${woke_minhook_SOURCE_DIR}/src/hook.c
    ${woke_minhook_SOURCE_DIR}/src/trampoline.c)
# src/ and src/hde/ are on the include path because MinHook's own sources mix relative
# ("../include/MinHook.h") and plain ("hde64.h") include forms across its files.
target_include_directories(minhook PUBLIC
    ${woke_minhook_SOURCE_DIR}/include
    ${woke_minhook_SOURCE_DIR}/src
    ${woke_minhook_SOURCE_DIR}/src/hde)
target_compile_definitions(minhook PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
