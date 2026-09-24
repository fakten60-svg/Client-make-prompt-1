cmake_minimum_required(VERSION 3.24)

project(woke
    VERSION 0.1.0
    DESCRIPTION "woke.wtf - native C++ injection utility client for Minecraft 1.21.11 Fabric"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Options ───────────────────────────────────────────────────────────────────
option(WOKE_BUILD_TESTS          "Build host-side logic tests (portable, no Windows needed)" ON)
option(WOKE_BUILD_INJECTOR       "Build the optional launcher/injector helper"                ON)
option(WOKE_WARNINGS_AS_ERRORS   "Treat first-party warnings as errors"                      OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# ── First-party DLL sources, added one roadmap step at a time ─────────────────
set(WOKE_CORE_SOURCES
    src/dllmain.cpp                  # Step 1: loader-lock-safe entry point
    src/core/lifecycle.cpp           # Step 1: single init/teardown owner
    src/core/logger.cpp              # Step 1: console + multi-session file logging
    src/utils/time_format.cpp        # Step 1: local-timezone timestamp formatting
    src/utils/win32_utils.cpp)       # Step 1: paths, console, module resolution

if(WIN32)
    include(ThirdParty)

    add_library(woke SHARED ${WOKE_CORE_SOURCES})
    target_include_directories(woke PRIVATE src)
    target_compile_definitions(woke PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX
        UNICODE _UNICODE
        WOKE_VERSION_STRING="${PROJECT_VERSION}")
    woke_apply_warnings(woke)

    # dwmapi is required by the Dear ImGui Win32 backend; psapi by MinHook's buffer code.
    target_link_libraries(woke PRIVATE
        imgui minhook nlohmann_json
        opengl32 user32 gdi32 shell32 ole32 dwmapi psapi dxgi d3d11)

    # Optimization contract: full program optimization + link-time code generation and
    # dead-code elimination. Debug keeps symbols for crash triage inside the JVM.
    # NOTE: one flag per generator expression - a single expression containing spaces
    # would be handed to cl.exe as one malformed argument.
    if(MSVC)
        target_compile_options(woke PRIVATE
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/GL>
            $<$<CONFIG:Release>:/GF>
            $<$<CONFIG:Release>:/Gy>
            $<$<CONFIG:Release>:/Zi>)
        target_link_options(woke PRIVATE
            $<$<CONFIG:Release>:/LTCG>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>)
    endif()

    set_target_properties(woke PROPERTIES OUTPUT_NAME "woke" PREFIX "")

    if(WOKE_BUILD_INJECTOR)
        add_executable(woke_injector launcher/injector.cpp)
        target_include_directories(woke_injector PRIVATE launcher)
        target_compile_definitions(woke_injector PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        woke_apply_warnings(woke_injector)
        set_target_properties(woke_injector PROPERTIES OUTPUT_NAME "woke_injector")
    endif()
else()
    message(STATUS
        "woke: non-Windows host detected. The DLL and injector require MSVC/x64 Windows "
        "and are skipped; building the portable host-side tests only.")
endif()

if(WOKE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
