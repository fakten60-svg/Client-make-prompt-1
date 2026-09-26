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

# Third-party acquisition runs on every host: nlohmann/json backs the portable mapping
# registry, so the host tests need it too. ImGui and MinHook stay Windows-only inside.
include(ThirdParty)

# ── First-party sources, added one roadmap step at a time ─────────────────────
# src/jni/mappings.cpp is deliberately portable (no jni.h, no windows.h, no logger): the
# mapping registry is the component a stale game update breaks first, so it is covered by
# the host tests rather than only by an in-game session.
set(WOKE_CORE_SOURCES
    src/dllmain.cpp                  # Step 1: loader-lock-safe entry point
    src/core/lifecycle.cpp           # Step 1: single init/teardown owner
    src/core/logger.cpp              # Step 1: console + multi-session file logging
    src/utils/time_format.cpp        # Step 1: local-timezone timestamp formatting
    src/utils/win32_utils.cpp        # Step 1: paths, console, module resolution
    src/jni/mappings.cpp             # Step 2: mappings.json registry (portable)
    src/core/event_bus.cpp           # Step 3: fixed-slot pub/sub (portable)
    src/core/config.cpp              # Step 5: settings persistence over nlohmann (portable)
    src/modules/module_manager.cpp   # Step 5: module registry, buckets, keybind routing
    src/modules/game_writes.cpp      # Step 7: the recording client-write backend (portable)
    src/modules/client_sound.cpp     # Step 8c: the Client Sound sink storage (portable)
    src/modules/client_sound_win32.cpp) # Step 8c: the Win32 PlaySound backend (Windows-only)

# Step 4 builds on the portable half of the UI too: the curves, the animation engine and the theme
# tokens carry no Windows dependency, so they are compiled by the host tests on Linux as well.
set(WOKE_UI_PORTABLE_SOURCES
    src/ui/theme.cpp                 # Step 4: palette, metrics and navigation tokens
    src/ui/animation/animation_controller.cpp) # Step 4: delta-time animations

set(WOKE_UI_SOURCES
    src/utils/render_utils.cpp       # Step 4: the only ImDrawList primitive layer
    src/ui/components/traffic_lights.cpp # Step 4: macOS window controls
    src/ui/components/pill_toggle.cpp    # Step 6: the animated Apple-style switch
    src/ui/hud.cpp                       # Step 7: the in-world overlay renderer (portable)
    src/ui/overlay.cpp                   # Step 7/8: the overlay policy predicate (portable)
    src/ui/step8_frame.cpp)              # Step 8: Combat/Mace readout fill (Windows-only)

# Step 6: the widget library. Every component here is portable (no windows.h), which is why the
# host tests compile them on Linux as well as into the DLL - the widgets are the part of the
# overlay a screenshot cannot check.
set(WOKE_UI_WIDGET_SOURCES
    src/ui/components/keybind_badge.cpp  # Step 6: the [KEY: X] chip + capture state machine
    src/ui/components/module_card.cpp    # Step 6: the slate module card + drawer
    src/ui/components/sidebar.cpp        # Step 6: navigation rows + counter badges
    src/ui/components/search_bar.cpp     # Step 6: the fixed-buffer module filter
    src/ui/notifications.cpp)            # Step 6: the fixed toast pool

set(WOKE_UI_GUI_SOURCES
    src/ui/gui.cpp)                  # Step 4/5/6: ImGui lifecycle, chrome, widget composition

if(WIN32)
    include(JavaHeaders)

    # Step 2: the JVM bridge. Compiled only when a JDK's jni.h was found; otherwise the
    # DLL still builds and lifecycle logs that the bridge is absent (WOKE_HAVE_JNI=0).
    set(WOKE_JNI_SOURCES "")
    if(WOKE_HAVE_JNI)
        list(APPEND WOKE_JNI_SOURCES
            src/jni/jni_context.cpp      # Step 2: VM discovery + per-thread attach
            src/jni/reflection_cache.cpp # Step 2: cached jclass/jmethodID/jfieldID lookups
            src/jni/game_instance.cpp    # Step 2: the only game-state façade
            src/jni/game_writes_jni.cpp) # Step 7: the JNI-backed gamma/fov write backend
    endif()

    # Step 3: the hook engine. Windows-only, but independent of the JNI bridge - the hooks
    # must work even when the mapping asset or the JDK is unavailable.
    set(WOKE_HOOK_SOURCES
        src/hooks/hook_manager.cpp       # Step 3: RAII MinHook wrapper + deferred removal
        src/hooks/swap_hook.cpp          # Step 3: wglSwapBuffers trampoline
        src/hooks/wndproc_hook.cpp       # Step 3: GLFW window subclass + input routing
        src/hooks/game_thread.cpp)       # Step 3: frame scheduler, tick gate, perf meter

    add_library(woke SHARED ${WOKE_CORE_SOURCES} ${WOKE_UI_PORTABLE_SOURCES} ${WOKE_JNI_SOURCES}
        ${WOKE_HOOK_SOURCES} ${WOKE_UI_SOURCES} ${WOKE_UI_WIDGET_SOURCES} ${WOKE_UI_GUI_SOURCES})
    target_include_directories(woke PRIVATE src)
    target_compile_definitions(woke PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX
        UNICODE _UNICODE
        WOKE_VERSION_STRING="${PROJECT_VERSION}")
    woke_apply_warnings(woke)

    # dwmapi is required by the Dear ImGui Win32 backend; psapi by MinHook's buffer code and
    # by the module scan that locates jvm.dll.
    # imgui is the core (host-independent) and imgui_backends is the Win32 + OpenGL3 glue; the
    # split is what lets the host tests render the draw layer headlessly on Linux.
    target_link_libraries(woke PRIVATE
        imgui imgui_backends minhook nlohmann_json
        opengl32 user32 gdi32 shell32 ole32 dwmapi psapi dxgi d3d11 winmm)

    # Spelled as two explicit branches rather than $<BOOL:...>: lifecycle.cpp switches its
    # boot steps on this macro, and an empty generator-expression operand is a needless
    # configure-time risk for one define.
    if(WOKE_HAVE_JNI)
        target_compile_definitions(woke PRIVATE WOKE_HAVE_JNI=1)
        target_link_libraries(woke PRIVATE jni_headers)
    else()
        target_compile_definitions(woke PRIVATE WOKE_HAVE_JNI=0)
    endif()

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
