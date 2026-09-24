# woke.wtf — Architectural Blueprint (Phase 1)

**Client:** woke.wtf — Native C++ Injection Utility Client
**Target:** Minecraft **1.21.11**, Fabric Loader, `javaw.exe` (x64 Windows)
**Artifact:** `woke.dll` — statically links Dear ImGui + MinHook + nlohmann/json
**Status:** Blueprint v1.0 — implementation in progress (Step 0 scaffold, Step 1 core foundation and Step 2 JVM bridge landed)
**Scope:** Private server utility testing, QoL automation, local singleplayer development. Zero public multiplayer servers. Strictly EULA-compliant educational/local use.

> **Interaction policy note (client-side only, by design):** every module operates through standard
> client-side game state read/write via cached JNI handles. The architecture contains **no** packet
> synthesis, no outbound traffic generation, no hidden/background actions, and no detection-evasion
> features. All module behavior is user-visible with explicit toggles. This is an architectural
> invariant, enforced at code review and listed in the review checklist (§11).

---

## Table of Contents

1. [Repository & Native Build Setup Guide](#1-repository--native-c-build-setup-guide)
2. [Complete Directory Tree](#2-complete-directory-tree)
3. [Per-File Module Breakdown](#3-per-file-module-breakdown)
4. [Core Systems Deep-Dive](#4-core-systems-deep-dive)
5. [JNI / Mappings Layer Contract](#5-jni--mappings-layer-contract)
6. [Thread-Safety & Memory Model](#6-thread-safety--memory-model)
7. [macOS ClickGUI, Animation Engine & Optimization Plan](#7-macos-clickgui-animation-engine--optimization-plan)
8. [Module Catalog](#8-module-catalog)
9. [Priority-Ordered Implementation Roadmap](#9-priority-ordered-implementation-roadmap)
10. [Development Workflow](#10-development-workflow)
11. [Code Review & Safety Checklist](#11-code-review--safety-checklist)
12. [Testing & Verification Strategy](#12-testing--verification-strategy)
13. [Risk Register & Error-Handling Matrix](#13-risk-register--error-handling-matrix)
14. [Decision Log](#14-decision-log)

---

## 1. Repository & Native C++ Build Setup Guide

### 1.1 Repository creation

Freebuff's managed GitHub credential authenticates all `git`/`gh` commands automatically — no PAT,
no SSH setup, no remote rewriting. Delivery (commits/pushes) goes through the Freebuff Changes
panel on explicit request. The commands below are the reference flow:

```bash
gh repo create woke-wtf --private --source=. --push      # on explicit go-ahead
git checkout -b develop && git push -u origin develop    # main = stable, develop = integration
```

### 1.2 Branching model

| Branch      | Purpose                                   | Merge rule                    |
|-------------|-------------------------------------------|-------------------------------|
| `main`      | Stable, tagged releases (`v0.x.x`)        | PR only, CI green required    |
| `develop`   | Integration branch                        | Squash-merge PRs              |
| `feature/*` | One roadmap step per branch               | → `develop`                   |
| `hotfix/*`  | Off `main`, back-merged both ways         | PR only                       |

Commit style: Conventional Commits — `feat:`, `fix:`, `perf:`, `refactor:`, `docs:`, `chore:`,
`build:`. One logical change per commit; roadmap step = one PR.

### 1.3 `.gitignore` (complete)

```gitignore
# Build artifacts
/build/
/bin/
/out/
*.dll
*.obj
*.pdb
*.lib
*.exp
*.ilk
*.idb
*.log

# IDE
/.vs/
.vscode/
.idea/
*.user
*.suo

# Runtime output
/logs/
/configs/

# OS
Thumbs.db
.DS_Store
```

### 1.4 Root `CMakeLists.txt` (complete reference)

```cmake
cmake_minimum_required(VERSION 3.24)
project(woke LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")

include(cmake/CompilerWarnings.cmake)   # /W4 (+ /WX in Release)

add_library(woke SHARED
    src/dllmain.cpp
    # ... sources added one file at a time per the roadmap (§9)
)
target_include_directories(woke PRIVATE src vendor)

# Statically vendored dependencies (pinned; see §1.6)
add_subdirectory(vendor/imgui EXCLUDE_FROM_ALL)    # win32 + opengl3 backends only
add_subdirectory(vendor/minhook EXCLUDE_FROM_ALL)
target_link_libraries(woke PRIVATE imgui minhook d3d11 dxgi opengl32 user32 kernel32)

# Optimization: full program optimization, LTCG, dead-code elimination
target_compile_options(woke PRIVATE
    $<$<CONFIG:Release>:/O2 /GL /GF /Gy /Zi>)
target_link_options(woke PRIVATE
    $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)

set_target_properties(woke PROPERTIES
    OUTPUT_NAME "woke"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin/$<CONFIG>")

# Host-side logic tests (easing, config round-trip, mappings parser) — §12
enable_testing()
add_subdirectory(tests EXCLUDE_FROM_ALL)
```

Locked build decisions:

- **Shared DLL**, x64 only, C++20, MSVC (VS 2022 toolset). `/MD` runtime (documented risk: CRT
  state inside the JVM process is fine because we never pass CRT objects across our boundary).
- **ImGui vendored at source level**, Win32 + OpenGL3 backends only (binary size + no runtime deps).
- **MinHook compiled from source** as a static sub-target.
- **nlohmann/json** single-header (`vendor/nlohmann/json.hpp`).
- `mappings.json` is a **runtime asset at repo root** — parsed at attach, never compiled in, so a
  game update is repaired by editing one file (see §5.4).

### 1.5 CI (GitHub Actions, `windows-latest`)

```yaml
name: ci
on: [push, pull_request]
jobs:
  build:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
        with: { submodules: recursive }
      - run: cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
      - run: cmake --build build --config Release
      - run: ctest --test-dir build -C Release --output-on-failure
      - uses: actions/upload-artifact@v4
        with: { name: woke-dll, path: bin/Release/woke.dll }
```

CI is the compile gate: the sandbox cannot run MSVC, so every roadmap step is verified by CI build +
host tests, then by in-game manual QA (§12.2).

### 1.6 Third-party vendoring (pinned, at repo scaffold time)

| Dependency     | Source                                   | Pin policy                    |
|----------------|------------------------------------------|-------------------------------|
| Dear ImGui     | github.com/ocornut/imgui (docking branch)| Pinned commit; submodules     |
| MinHook        | github.com/TsudaKageyu/minhook           | Pinned commit; submodules     |
| nlohmann/json  | github.com/nlohmann/json (single header) | Pinned release tag            |
| JNI headers    | JDK 21 (`jni.h`, `jni_md.h`)             | System include via `JAVA_HOME` |

---

## 2. Complete Directory Tree

```
woke.wtf/
├── CMakeLists.txt                     # Root build script (§1.4)
├── cmake/
│   └── CompilerWarnings.cmake         # /W4 centrally; /WX Release; per-lib suppressions
├── .gitignore                         # §1.3
├── mappings.json                      # PROVIDED (absent in workspace today — §5.4 contract)
├── README.md                          # Preserved untouched; rewritten at Step 10
├── vendor/                            # Pinned third-party sources (§1.6)
│   ├── imgui/
│   ├── minhook/
│   └── nlohmann/
├── launcher/
│   ├── injector.h                     # Optional LoadLibraryW injector interface
│   └── injector.cpp                   # Read decision: included or dropped (§14, D-01)
├── tests/
│   ├── CMakeLists.txt                 # woke_tests console target (runs in CI)
│   ├── test_easing.cpp                # Pure-function tests: lerp/easing/spring invariants
│   ├── test_config_roundtrip.cpp      # Settings → JSON → Settings identity
│   └── test_mappings_parser.cpp       # mappings.json schema variants + defensive fallbacks
├── configs/                           # Runtime output (gitignored)
├── logs/                              # Runtime output (gitignored)
└── src/
    ├── dllmain.cpp                    # DLL_PROCESS_ATTACH → single worker thread
    │
    ├── core/                          # ── Framework core ──
    │   ├── version.h                  # Build metadata (version, branch, build date)
    │   ├── lifecycle.h / .cpp         # Startup/shutdown orchestrator — the only init owner
    │   ├── event_bus.h / .cpp         # Type-safe fixed-slot pub/sub (§4.1)
    │   ├── events.h                   # Event payload structs (FrameEvent, KeyEvent, …)
    │   ├── logger.h / .cpp            # ANSI console + session file + latest.log (§4.3)
    │   └── config.h / .cpp            # JSON persistence bound to every BaseSetting<T> (§4.2)
    │
    ├── jni/                           # ── JVM interop layer ──
    │   ├── jni_context.h / .cpp       # VM discovery, scoped attach, local-frame RAII (§5.1)
    │   ├── mappings.h / .cpp          # mappings.json loader → hash registry (§5.4)
    │   ├── reflection_cache.h / .cpp  # jclass/jmethodID/jfieldID one-line lookups (§5.2)
    │   ├── game_instance.h / .cpp     # Live mc/player/world global refs (§5.3)
    │   └── game_types.h               # C++ value mirrors: Vec3d, BlockPos, hand/slot enums
    │
    ├── hooks/                         # ── Native hooking engine ──
    │   ├── hook_manager.h / .cpp      # RAII MinHook wrapper + queued self-removal (§4.5)
    │   ├── swap_hook.h / .cpp         # wglSwapBuffers trampoline; DXGI fallback flag (§4.6)
    │   ├── wndproc_hook.h / .cpp      # GLFW HWND subclass → key/mouse events (§4.7)
    │   └── game_thread.h / .cpp       # Frame scheduler + 20 Hz tick gate (§4.8)
    │
    ├── settings/                      # ── Generic setting primitives ──
    │   ├── base_setting.h             # BaseSetting<T>: value/bounds/dirty/serialize (§4.2)
    │   ├── setting_types.h            # Bool, Slider, Color, Keybind, Mode
    │   └── setting_serializer.h       # nlohmann ADL binding used by config engine
    │
    ├── modules/                       # ── Utility modules ──
    │   ├── category.h                 # enum Category { Combat, Mace, Misc, Movement, Spear, Visual }
    │   ├── base_module.h              # Virtual lifecycle: on_enable/on_disable/on_tick/on_render
    │   ├── module_manager.h / .cpp    # Registry, category buckets, fan-out, needs_render (§4.4)
    │   ├── combat/                    # target_hud, attack_cooldown, reach_display, combat_stats
    │   ├── mace/                      # smash_potential, wind_charge_cd, mace_stats, smash_flash
    │   ├── misc/                      # clickgui_open, config_hotkeys, friend_manager, client_sound, panic
    │   ├── movement/                  # auto_sprint, safe_walk, velocity_display
    │   ├── spear/                     # riptide_indicator, trident_cd, loyalty_hud
    │   └── visual/                    # fullbright, hud, zoom, trajectories, custom_crosshair
    │
    ├── ui/                            # ── macOS ClickGUI + animation engine ──
    │   ├── theme.h / .cpp             # Palette + ImGui style tokens (§7.2)
    │   ├── gui.h / .cpp               # macOS window: chrome, sidebar, header, card area (§7.1)
    │   ├── hud.h / .cpp               # In-game overlay: watermark, arraylist, module HUDs (§7.6)
    │   ├── notifications.h / .cpp     # Fixed-pool toast manager (§7.5)
    │   ├── animation/
    │   │   ├── animation_controller.h # dt-based AnimState registry (§7.4)
    │   │   └── easing.h               # constexpr lerp / easeOutCubic / easeOutExpo / spring
    │   └── components/                # Reusable BaseUIComponent widgets (§7.3)
    │       ├── base_component.h       # render() / handle_input() / animate() interface
    │       ├── traffic_lights.h/.cpp  # Close/minimize/zoom circular buttons
    │       ├── pill_toggle.h/.cpp     # Apple-style animated switch
    │       ├── module_card.h/.cpp     # Slate card + keybind badge + chevron + drawer
    │       ├── sidebar.h/.cpp         # Nav sections + counter badges
    │       ├── search_bar.h/.cpp      # Fixed-buffer input, filters module list
    │       └── keybind_badge.h/.cpp   # [KEY: X] chip + capture mode
    │
    └── utils/                         # ── Stateless libraries ──
        ├── math_utils.h               # constexpr lerp, easing, vec ops, color transforms
        ├── render_utils.h / .cpp      # Rounded rects, shadows, gradients, text clipping (§7.7)
        ├── string_buffer.h            # FixedString<N> + fmt_into — zero-heap text (§6.3)
        └── win32_utils.h / .cpp       # ANSI console setup, module/proc resolution, paths
```

~55 files, every file 100–200 lines. Nothing lives outside this tree.

---

## 3. Per-File Module Breakdown

Every entry states: role, reusability, game-state interaction mode, and `mappings.json` integration.
*(Interaction mode: **R** = read game state, **W** = write client-side state, **N** = no game state.)*

### 3.1 Entry point & core

| File | Role & reusability | Mode | mappings.json |
|---|---|---|---|
| `dllmain.cpp` | `DLL_PROCESS_ATTACH` spawns exactly one worker thread (never blocks the loader lock); thread runs `lifecycle::boot()` then parks waiting on the hot-unload signal. Zero other work. | N | — |
| `core/version.h` | Single source of UI/log version metadata (`WOKE_VERSION`, branch, build timestamp) consumed by logger, sidebar logo, and watermark. | N | — |
| `core/lifecycle.cpp` | The **only** init-order owner: logger → mappings → JNI cache → hooks → ImGui → modules → configs; teardown in exact reverse. No system self-initializes in static constructors, making boot deterministic and hot-unload clean. | N | passes loader to cache |
| `core/event_bus.h/.cpp` | Type-safe fixed-slot pub/sub (§4.1): `bus.subscribe<KeyEvent>(slot, fn)`; `post<T>` is a linear dispatch over pre-sized slots. Decouples modules from each other — they subscribe to events, never hold pointers to systems. | N | — |
| `core/events.h` | Plain structs (`FrameEvent{dt,frame}`, `TickEvent`, `KeyEvent{vk,down,consumed}`, `MouseEvent`, `WorldChangeEvent`, `PlayerChangeEvent`, `ConfigLoadedEvent`, `ShutdownEvent`) — the complete vocabulary of cross-system communication. | N | — |
| `core/logger.cpp` | ANSI color-coded Win32 console (`DEBUG` gray / `INFO` cyan / `WARN` yellow / `ERROR` red), local-PC-timezone `[YYYY-MM-DD HH:MM:SS.mmm]` via `localtime_s`, dual write to `/logs/<datetime>.log` (append) and `/logs/latest.log` (overwrite-mirror) per §4.3. Preallocated format buffers. | N | — |
| `core/config.cpp` | Walks `ModuleManager::all_settings()` and round-trips `/configs/<name>.json` via nlohmann; settings auto-participate by existing — new modules need zero config code. Load errors are logged, never fatal; unknown keys are ignored (forward compatibility). | N | — |

### 3.2 JNI / mappings layer

| File | Role & reusability | Mode | mappings.json |
|---|---|---|---|
| `jni/jni_context.cpp` | Discovers the JVM (`JNI_GetCreatedJavaVMs`), RAII `AttachCurrentThreadAsDaemon` per thread, and `PushLocalFrame/PopLocalFrame` scoped guards so per-frame local refs can never leak (§5.1). All other JNI code consumes these guards. | N | — |
| `jni/mappings.cpp` | Parses `mappings.json` once at boot into a `unordered_map<string, MappedEntry>`; tolerant of the schema variants in §5.4; unresolved entries log `WARN` once and yield invalid handles — never a crash. | N | **primary consumer** |
| `jni/reflection_cache.cpp` | `class_of("class_310")`, `method_of("class_310","method_1554")`, `field_of(...)` one-liners returning cached handles; `FindClass`/`GetMethodID`/`GetFieldID` each execute exactly once per key for the process lifetime (§5.2). | N | keys come from here |
| `jni/game_instance.cpp` | Resolves and holds global refs to the live `MinecraftClient` and derived `player`/`world`/`options` handles; re-resolves on world/player change events; every module reads game state through this façade, never raw JNI. | R | intermediary IDs |
| `jni/game_types.h` | POD value mirrors (`Vec3d`, `BlockPos`, hand/slot enums) converted from JNI calls at the boundary — modules stay JNI-free and allocation-free. | R | — |

### 3.3 Hook engine

| File | Role & reusability | Mode | mappings.json |
|---|---|---|---|
| `hooks/hook_manager.cpp` | Thin RAII over MinHook (`create/enable/queue_remove`) with deferred removal so a hook can safely unhook from inside its own trampoline; owns a single `MH_Uninitialize` at teardown. | N | — |
| `hooks/swap_hook.cpp` | `wglSwapBuffers` trampoline (resolved via `wglGetProcAddress`/`opengl32` exports; DXGI `Present` behind a compile flag). Because it fires **on the game thread**, it doubles as the frame scheduler entry (§4.6). | N | — |
| `hooks/wndproc_hook.cpp` | Resolves the GLFW window's `HWND` (`GetProcAddress` on `lwjgl_glfw.dll` for `glfwGetWin32Window`, EnumWindows fallback) and subclasses `WndProc` to feed keys/mouse to the event bus and ImGui's Win32 backend. | N | — |
| `hooks/game_thread.cpp` | Per-swap scheduler: advances `AnimationController`, drains nothing from other threads (by design), fires `FrameEvent`, gates `TickEvent` at 20 Hz from frame timing, and guards the overlay with the <0.5 ms perf meter (§4.8). | N | — |

### 3.4 Settings & module system

| File | Role & reusability | Mode | mappings.json |
|---|---|---|---|
| `settings/base_setting.h` | `BaseSetting<T>`: name, description, value, default, dirty flag, pure-virtual `serialize/deserialize`; the single template every UI widget and the config engine operates on. | N | — |
| `settings/setting_types.h` | `BoolSetting`, `SliderSetting<T>`, `ColorSetting`, `KeybindSetting` (VK + modifiers, capture mode), `ModeSetting` (fixed `const char*` list) — all derivations, no duplication. | N | — |
| `settings/setting_serializer.h` | nlohmann ADL overloads per concrete type — the only file that knows both settings and JSON. | N | — |
| `modules/category.h` | The six-category enum + display metadata; single point of truth for sidebar sections and bucketing. | N | — |
| `modules/base_module.h` | `BaseModule`: `name/description/category/settings[]/keybind` + virtual `on_enable/on_disable/on_tick/on_render`; enable/disable is the only mutation path and always emits a toast; `requires_mappings` gate auto-disables a module whose intermediary IDs failed to resolve (defensive, §5.4). | N | gate flag |
| `modules/module_manager.cpp` | Fixed pool of modules, category buckets (badge counts precomputed), keybind dispatch, per-frame fan-out behind one cached `needs_render()` bool that drives draw-call suppression (§4.4, §7.8). | W | via instances |

### 3.5 Modules (interaction summary; full specs §8)

All modules inherit the same `BaseModule` + `BaseSetting<T>` stack, read game state exclusively via
`game_instance` cached handles (keys from `mappings.json`), and write only **client-side** state
(options/render fields/local UI). **No module generates or mutates network packets** — this is an
architectural invariant, not a convention.

- `combat/` — `target_hud` (R: looked-at entity → HUD card), `attack_cooldown` (R: cooldown progress → bar), `reach_display` (R: client reach readout), `combat_stats` (R: session counters).
- `mace/` — `smash_potential` (R: fall distance → projected damage readout), `wind_charge_cd` (R: cooldown → HUD), `mace_stats` (R: session stats), `smash_flash` (R → local screen-edge flash, visual only).
- `misc/` — `clickgui_open` (N: bind module), `config_hotkeys` (N: load/save binds), `friend_manager` (N: local nametag color list), `client_sound` (N: local UI clicks via `PlaySound`), `panic` (N: disable-all keybind).
- `movement/` — `auto_sprint` (W: visible auto-sprint toggle), `safe_walk` (W: client-side edge assist), `velocity_display` (R: knockback readout). Final list confirmed at implementation (D-02).
- `spear/` — `riptide_indicator` (R), `trident_cd` (R), `loyalty_hud` (R: return-trip readout).
- `visual/` — `fullbright` (W: gamma option field), `hud` (N: watermark/arraylist), `zoom` (W: smooth FOV option lerp), `trajectories` (R: pure client-side projectile math overlay), `custom_crosshair` (N: local overlay).

### 3.6 UI & utils

| File | Role & reusability | Mode | mappings.json |
|---|---|---|---|
| `ui/theme.cpp` | All palette constants + ImGui style table in one place (§7.2) — widgets and modules reference tokens, never raw hex, so a re-theme is one file. | N | — |
| `ui/gui.cpp` | The macOS window composition: custom chrome (traffic lights, centered title), sidebar, header with search + view toggle, card grid/list. Contains **no** widget drawing — it composes components (§7.1). | N | — |
| `ui/hud.cpp` | In-game overlay: watermark (version + fps), arraylist of enabled modules (animated reordering), per-module HUD fragments rendered via the same component stack as the GUI. | N | — |
| `ui/notifications.cpp` | Fixed pool of 8 toasts: `push({title, message, icon})` by value; owns queueing, layout, slide-in, progress bar, slide-out (§7.5). | N | — |
| `ui/animation/animation_controller.h` | Central dt-based `AnimState` registry with fixed slot arrays; widgets acquire handles at creation, no map lookups per frame (§7.4). | N | — |
| `ui/animation/easing.h` | `constexpr` curves (`easeOutCubic`, `easeOutExpo`, spring-damper, `lerp`, `lerpColor`) shared by GUI, HUD, toasts, and tests. | N | — |
| `ui/components/base_component.h` | `BaseUIComponent` interface (`render()/handle_input()/animate()`) + per-component `AnimState` handles — the contract that makes every widget reusable in GUI, HUD, and toasts. | N | — |
| `ui/components/traffic_lights.cpp` | Three circular buttons (`#FF5F56/#FFBD2E/#27C93F`) with eased hover brightness; close hides the GUI (does not unload), minimize animates scale-to-pill, zoom toggles grid/list density (§7.1). | N | — |
| `ui/components/pill_toggle.cpp` | One animated `t ∈ [0,1]` drives knob X and dark-gray→Apple-blue cross-fade simultaneously — they cannot desync (§7.4). | N | — |
| `ui/components/module_card.cpp` | Slate card: white title, muted-gray description, `keybind_badge`, chevron eased 0°→90° for the settings drawer, right-aligned `pill_toggle`. Reused identically in list and grid layout. | N | — |
| `ui/components/sidebar.cpp` | Logo + version block, MODULES section (six categories, active-count badges), GENERAL section (Settings/Theme/Configs/Socials/Keybinds); hover brightness + selection live in the component. | N | — |
| `ui/components/search_bar.cpp` | 64-char fixed buffer `InputText`; filter result count and match state computed on **input change**, not per frame. | N | — |
| `ui/components/keybind_badge.cpp` | `[KEY: X]` chip; click enters capture mode (next WndProc key rebinds, `ESC` cancels) — one capture flag owned here. | N | — |
| `utils/math_utils.h` | Header-inline `constexpr` math: `lerp`, easing, vec ops, HSV/RGBA transforms; zero storage, zero allocation; directly unit-tested (§12). | N | — |
| `utils/render_utils.cpp` | Stateless `ImDrawList` helpers — rounded rect with soft shadow (4-segment draw, not layered rects), gradient fill, border stroke, clipped text — the only place ImGui primitives are touched. | N | — |
| `utils/string_buffer.h` | `FixedString<N>` + `fmt_into()` used by every per-frame text draw; makes per-frame heap allocation a compile-time impossibility for text (§6.3). | N | — |
| `utils/win32_utils.cpp` | ANSI console enablement, `GetModuleHandle`/`GetProcAddress` helpers, exe-directory path resolution for `logs/`, `configs/`, `mappings.json`. | N | — |
| `launcher/injector.cpp` | *Optional* minimal `LoadLibraryW` injector for local singleplayer testing (D-01 pending). | N | — |

---

## 4. Core Systems Deep-Dive

### 4.1 Event bus (`core/event_bus`)

- **Type-safe:** events are structs; `post<KeyEvent>(e)` dispatches only to `KeyEvent` subscribers
  via compile-time slot tables — no `void*`, no dynamic casts.
- **Fixed slots:** each event type has `std::array<Handler, 16>` pre-sized at boot; `subscribe` fills
  the next free slot; posting is a linear sweep of registered slots. **Zero allocation at rest and at
  post time.**
- **Ownership:** the bus holds function pointers + context pointers, never `std::function` churn.
  Unsubscribe at module disable swaps the slot's handler to nullptr (O(1)).
- **Threading:** all posts happen on the game thread (§6) — no locks, no atomics needed.

### 4.2 Config engine (`core/config`) + settings

- Every `BaseSetting<T>` registers itself into its owner module's fixed settings array at
  construction; `ModuleManager::all_settings()` is a precomputed list assembled once at boot.
- `config::save(name)` → `/configs/<name>.json` = `{ "module": { "setting": value } }` via the
  `setting_serializer` ADL layer. `config::load(name)` is tolerant: missing keys keep defaults,
  unknown keys are ignored and logged at `DEBUG`.
- **Auto-binding invariant:** adding a module with new settings requires **zero** config code —
  persistence is a property of the setting, not a registration step.
- Config IO runs on the worker thread (post-boot), never in `on_tick`/`on_render`.

### 4.3 Logging engine (`core/logger`)

- **Console:** `SetConsoleMode` + VT processing → ANSI codes; `DEBUG` gray `#8A8F98`, `INFO` cyan
  `#3FD2E0`, `WARN` yellow `#FFBD2E`, `ERROR` red `#FF5F56` (matches GUI accent palette).
- **Timestamps:** local PC timezone via `localtime_s` + `GetLocalTime`, formatted
  `[YYYY-MM-DD HH:MM:SS.mmm]` into a stack buffer — no `std::string`, no locale dependence.
- **Session files:** on attach, `logs/<YYYY-MM-DD_HH-MM-SS>.log` is created (local time, e.g.
  `logs/2026-09-20_15-41-00.log`); `logs/latest.log` is truncated and mirrored line-for-line
  simultaneously (single `fwrite` to both streams per line, single `fflush` at `ERROR` level).
- **Retention:** files older than 20 sessions are deleted at boot (keeps low-end disks clean).
- Thread rule: game thread logs are buffered into a fixed SPSC ring (256 entries, preallocated) and
  flushed by the worker thread — logging never blocks a frame.

### 4.4 Module manager (`modules/module_manager`)

- Fixed pool (`std::array<ModulePtr, 32>` — raw `unique_ptr` storage filled at boot, never resized).
- Category buckets precompute: per-category total + enabled counts → sidebar badges are two int
  reads, recomputed only on enable/disable.
- **Keybind dispatch:** `KeyEvent` → match `KeybindSetting` → toggle (press-mode) or hold
  (hold-mode, movement modules). GUI-open suppresses module binds except `clickgui_open`.
- **Fan-out:** `FrameEvent` → for each enabled module `on_tick(dt)` (20 Hz gate) and `on_render(dt)`;
  a single cached bool `needs_render()` (GUI open ∨ toasts alive ∨ HUD modules enabled) drives
  draw-call suppression (§7.8).
- Enable/disable always: logs + toast + persist-dirty flag. Module constructor never touches JNI.

### 4.5 Hook manager (`hooks/hook_manager`)

- RAII `Hook { target, detour, original }`; creation/enable/disable wrapped with `MH_STATUS`
  checks logged at `ERROR` with the hook name.
- **Deferred self-removal:** `queue_remove()` marks the hook and executes `MH_RemoveHook` from the
  next safe point — a trampoline can request its own teardown without deadlocking MinHook.
- Teardown order (hot-unload): unsubclass WndProc → ImGui shutdown → `MH_Uninitialize` → JNI detach
  → close logs. Reverse of boot, verified by the Step 9 gate.

### 4.6 Swap hook (`hooks/swap_hook`)

- Target: `wglSwapBuffers` (Minecraft 1.21.11 renders through LWJGL/OpenGL). Resolution:
  `GetModuleHandleW(L"opengl32.dll")` + `GetProcAddress`; if the context differs at runtime, the
  DXGI `Present` path compiles in behind `WOKE_USE_DXGI` (D-03 locked: OpenGL primary).
- Trampoline body: `if (!needs_render) return original(...)` — **the zero-overhead path is one
  branch**; otherwise run the frame pipeline (§4.8) then `return original(...)`.
- Runs on the game thread ⇒ all JNI game-state access is inherently serialized with the game
  (§6) — no `mc.execute()` reflection dispatch, no Java-side method hooks required.

### 4.7 WndProc hook (`hooks/wndproc_hook`)

- `glfwGetWin32Window` resolved from `lwjgl_glfw.dll` (fallback: EnumWindows matching the process
  window class) → `SetWindowLongPtrW(GWLP_WNDPROC)` subclass; restored on teardown before ImGui
  shutdown.
- Routes: `WM_KEYDOWN/UP/SYSKEYDOWN` → `KeyEvent` (ImGui backend + keybind dispatch, `consumed`
  flag stops module binds when GUI handles the key); mouse msgs → `MouseEvent`; `WM_KILLFOCUS` →
  auto-close GUI (macOS-style behavior).
- NumLock/ScrollLock state is captured at boot so `GetAsyncKeyState` parity quirks can't corrupt
  modifier state.

### 4.8 Game thread scheduler (`hooks/game_thread`)

Per swap, in order:

1. `animation_controller.tick(dt)` — all UI motion advances once per frame.
2. `FrameEvent` post → module `on_render` fan-out + GUI/HUD/notifications draw.
3. 20 Hz gate (`dt accumulator`) → `TickEvent` → module `on_tick` (state reads/writes).
4. Overlay perf meter: `QueryPerformanceCounter` around steps 1–3; rolling 120-frame average;
   sustained >0.5 ms for 30 frames → throttled `WARN` (≤1 per 10 s).

### 4.9 Lifecycle (`core/lifecycle`)

Boot order (each step logs `INFO` + duration in ms):

```
logger → win32_utils → mappings → jni_context → reflection_cache → game_instance
      → hook_manager → swap_hook → wndproc_hook → theme → animation → gui/hud
      → notifications → modules → config::load(default) → "boot complete"
```

Shutdown is exact reverse; any step failing logs `ERROR` and skips dependent steps (defensive boot —
a missing JVM or stale mapping degrades to a console-only DLL, never a crash).

---

## 5. JNI / Mappings Layer Contract

### 5.1 `jni_context` — scoped attach discipline

- Discover `JavaVM*` once via `JNI_GetCreatedJavaVMs` resolved dynamically from the `jvm.dll` the
  game already loaded (never an import library, so the DLL stays loadable in any process); refuse
  multi-VM (log `ERROR`).
- **Class discovery is classloader-first, in that order:** `FindClass` on the system loader is tried
  first and the game's loader is the fallback. Minecraft's classes live in Knot's loader, and a
  thread we created ourselves inherits no useful context loader, so the fallback takes the context
  class loader of a live Java thread (`Thread.enumerate`) once and caches it as a global ref. Without
  this the bridge attaches successfully and then resolves nothing.
- `ScopedAttach` RAII: `AttachCurrentThreadAsDaemon` (idempotent if already attached) + `env`.
- `ScopedLocalFrame` RAII: `PushLocalFrame(16)` / `PopLocalFrame` — per-frame JNI locals are
  released as a block; **no `DeleteLocalRef` bookkeeping anywhere else in the codebase**.
- All global refs (`NewGlobalRef`) are owned exclusively by `game_instance`/`reflection_cache`,
  released at teardown in reverse resolution order.

### 5.2 `reflection_cache` — one-line lookups

```cpp
jclass    cls = jni::class_of("class_310");              // cached jclass (global ref)
jmethodID m   = jni::method_of("class_310", "method_1551"); // MinecraftClient.getInstance
jfieldID  fid = jni::field_of("class_746", "field_1724");   // player field on the client
```

- Internally: hash lookup `mappings.cpp` registry → first call resolves via `FindClass` /
  `GetMethodID` / `GetFieldID` → stored; every later call is a hash hit and a pointer read.
- **Invariant:** `FindClass`/`GetMethodID`/`GetFieldID` appear **only** in this file. CI grep gate
  (§11) enforces it.
- Invalid handles return a shared `JNI_INVALID` sentinel checked by call sites with a single `if` —
  modules whose required IDs failed to resolve are auto-disabled with a `WARN` toast, not crashed.

### 5.3 `game_instance` — the only game-state façade

- Caches: `MinecraftClient` instance (intermediary `class_310`), `player` (`class_746`),
  `world` (`class_638`), `options` (`class_315`) — resolved once, refreshed on
  `WorldChangeEvent`/`PlayerChangeEvent`.
- Typed accessors return `game_types` PODs (`Vec3d`, cooldown floats, option values) — conversion
  happens at this boundary; **modules never see `JNIEnv`**.
- Null world/player → accessors return `std::nullopt`-style POD with valid flag; module code is a
  single guard, identical shape everywhere.

### 5.4 `mappings.json` — schema contract & defensive loading

The asset is generated from the authoritative upstream source instead of being written by hand.
`tools/generate_mappings.py` reads the official Yarn archive and emits the asset:

```bash
curl -sL -o build/tmp/yarn-1.21.11+build.6-v2.jar \
  https://maven.fabricmc.net/net/fabricmc/yarn/1.21.11+build.6/yarn-1.21.11+build.6-v2.jar
python3 tools/generate_mappings.py --jar build/tmp/yarn-1.21.11+build.6-v2.jar \
  --version 1.21.11 --source net.fabricmc:yarn:1.21.11+build.6 --out mappings.json
```

Current asset: **39 classes / 2726 methods / 1251 fields** (461 KB), produced from
`net.fabricmc:yarn:1.21.11+build.6`. Every curated class resolved; the generator prints a warning
list instead of failing when upstream renames one. Emitted schema (real values, not examples):

```json
{
  "version": "1.21.11",
  "namespace": "intermediary",
  "source": "net.fabricmc:yarn:1.21.11+build.6",
  "classes": {
    "MinecraftClient": {
      "yarn": "net/minecraft/client/MinecraftClient",
      "intermediary": "net/minecraft/class_310",
      "aliases": ["MinecraftClient", "class_310", "net/minecraft/class_310"],
      "methods": {
        "getInstance": { "intermediary": "method_1551", "descriptor": "()Lnet/minecraft/class_310;" }
      },
      "fields": {
        "player": { "intermediary": "field_1724", "descriptor": "Lnet/minecraft/class_746;" }
      }
    }
  }
}
```

Member descriptors are expressed with intermediary class names, which is exactly what
`GetMethodID`/`GetFieldID` require at runtime, so the JNI layer never has to build a signature by
hand. Intermediary member names are **not** always `method_XXXX`/`field_XXXX`: recent Yarn builds
also emit `comp_XXXX` for record components, so nothing in the client may assume a name prefix.
Aliases index the same entry, which is what lets §5.2 look classes up by short intermediary key.

Loader tolerance (all variants accepted, parsed once at boot):

1. Map keyed by friendly name (canonical, above).
2. Map keyed by intermediary (`"net/minecraft/class_310"`), friendly name from an optional
   `"name"` field.
3. Array of entries `{ "name": "...", "intermediary": "...", "methods": {…}, "fields": {…} }`.
4. Method/field values as either objects or plain `"method_XXXX"` strings.

Failure policy: missing file → `ERROR` log, all handles invalid, JNI modules auto-disable (client
still boots, GUI works, config works). Unknown key → one-time `WARN`. Verified anchors asserted at
boot with an `INFO` line (`mappings: resolved N classes / M methods / K fields`) include
`class_310` (MinecraftClient), `class_746` (ClientPlayerEntity), `class_638` (ClientWorld),
`class_315` (GameOptions), plus the spot-checked members `method_1551` (getInstance),
`field_1724` (player) and `field_1687` (world). **IDs are read from generated data, never guessed;
a game update is repaired by regenerating the asset (`tools/generate_mappings.py`) and adding any
new class to its curated list.**

---

## 6. Thread-Safety & Memory Model

### 6.1 Threads (exactly two)

| Thread | Created by | Touches JNI game state? | Work |
|---|---|---|---|
| **Game thread** (existing MC thread) | — | **Yes — exclusively** | Swap trampoline: animation tick, `FrameEvent`, 20 Hz `TickEvent`, GUI/HUD draw |
| **Worker thread** (ours) | `dllmain` | **Never** | Boot orchestration, config file IO, log flushing, unload wait |

### 6.2 Why no task queue into Java

`wglSwapBuffers` fires on Minecraft's client thread, so scheduling all game-state access there makes
every JNI touch inherently serialized with the game — the entire class of concurrent-modification
crashes is eliminated **without** a `mc.execute()` reflection bridge or JVM method hooks. The
user-spec's "task queue dispatched onto the main thread" is satisfied *structurally* (the swap hook
*is* the main-thread dispatch point) and simplifies to zero cross-thread handoff. Locked: D-04.

### 6.3 Zero-allocation discipline (enforced, not aspirational)

- **Text:** `FixedString<N>` + `fmt_into` everywhere in hot paths; `std::string` forbidden in
  `on_tick`/`on_render` (grep-gated, §11).
- **Containers:** module/settings/toast/handler pools are `std::array` or `reserve`d once at boot;
  no container growth after `lifecycle::boot()` returns.
- **JNI:** locals bounded by `ScopedLocalFrame`; globals owned in two places only (§5.1); no
  `NewObject` in hot paths — reads convert into stack PODs.
- **ImGui:** vertices go through ImGui's arena which is sized once; we never create/destroy ImGui
  contexts or fonts after boot.
- **Debug enforcement:** Debug builds compile `_CRTDBG_MAP_ALLOC` + a `WOKE_NOALLOC` define that
  routes hot-path `operator new` to a fatal breakpoint; the Step 9 perf pass runs a 10-minute
  singleplayer soak with zero new allocations asserted.

### 6.4 Perf budget (<0.5 ms overlay)

| Component | Budget |
|---|---|
| Animation tick (all states) | 0.02 ms |
| GUI closed path (branch only) | ~0.00 ms |
| HUD watermark + arraylist | 0.08 ms |
| ClickGUI open (full frame) | 0.40 ms |
| Toast draw (≤8) | 0.05 ms |
| JNI reads per tick gate | 0.03 ms |

Instrumented by the scheduler (§4.8); the meter is itself allocation-free.

---

## 7. macOS ClickGUI, Animation Engine & Optimization Plan

### 7.1 Window & layout (concrete metrics)

- ImGui window: `NoDecoration | NoMove | NoResize | NoBringToFrontOnFocus`, position via
  `SetWindowPos` (centered, draggable by our own header hit-test).
- Base size **980 × 620**; sidebar **220 px**; header **64 px**; content padding **20 px**.
- Backdrop `#0B0E14` at **90 % alpha**, `window_rounding = 14.0`, `frame_rounding = 8.0`,
  `#2A3548` 1 px border stroke, soft drop shadow via `render_utils::shadow_rect` (single
  4-segment draw-list call, not stacked translucent rects).
- Card grid: 2 columns × N; card **340 × 92** (grid) or full-width **92** rows (list). Cards carry:
  white title, `#8A96A8` description, `keybind_badge`, eased chevron, right-aligned `pill_toggle`.
- Header: category title ("Mace Modules"), sub-badge `"%d modules · %d enabled"` formatted **once
  per state change** into a `FixedString` (never per frame), `search_bar` (right), list/grid toggle.
- Sidebar: logo "woke.wtf" + `version.h` metadata; MODULES: Combat, Mace, Misc, Movement, Spear,
  Visual — each with an active-count badge; GENERAL: Settings, Theme, Configs, Socials, Keybinds.

### 7.2 Theme tokens (`ui/theme`)

| Token | Value |
|---|---|
| Window backdrop | `#0B0E14` @ 0.90 |
| Border stroke | `#2A3548` |
| Card base / hover | `#171C28` → `#1D2434` (eased) |
| Title / muted text | `#FFFFFF` / `#8A96A8` |
| Accent (toggles, active nav) | Apple blue `#0A84FF`, cyan `#3FD2E0` |
| Traffic lights | `#FF5F56`, `#FFBD2E`, `#27C93F` |
| Roundings | window 14.0, frame/card 8.0, pill height 22, knob Ø 18 |

### 7.3 Component contract (`BaseUIComponent`)

```cpp
struct BaseUIComponent {
    virtual void render(ImDrawList* dl, const Rect& r) = 0;   // draw only
    virtual bool handle_input(const Input& in) = 0;           // returns true = consumed
    virtual void animate(float dt) = 0;                       // advance own AnimStates
};
```

Every widget (traffic lights, pill, card, sidebar row, search, badge, toast) implements this exact
trio, holds its own `AnimState` handles, and is therefore embeddable in GUI, HUD, and toasts with no
adaptation. `render` never mutates state; `animate` never draws; input is consumed top-down.

### 7.4 Animation engine

- `AnimState { float value; float target; float speed; }` in fixed arrays owned by
  `AnimationController`; `tick(dt)` advances all states with **delta time** — motion identical at
  60 or 240 FPS. Widgets acquire handle indices at creation; no map lookups per frame.
- Curves (`easing.h`, `constexpr`): `easeOutCubic` (toggles, chevrons), `easeOutExpo` (window
  scale), critically-damped spring (GUI open/close), `lerp`/`lerpColor` (positions and cross-fades).
- **Pill toggle:** one animated `t ∈ [0,1]` drives knob X (`lerp`) and background color
  (`lerpColor(dark_gray, apple_blue, t)`) simultaneously — they cannot desync.
- **GUI open/close:** spring scale 0.94 → 1.00 + alpha 0 → 1 (~180 ms, easeOutExpo); close mirrors
  it, then suppression kicks in (§7.8). Triggered by bind, `clickgui_open` module, or red button
  (hide) / `panic` (disable-all).
- **Hover:** brightness multiplier 1.00 → 1.25 eased per component (cards, nav rows, traffic
  lights, badges) — one AnimState per component, no shared/global hover state.
- **Toast:** slide-in from top-right (40 px, easeOutCubic), 3 s life, 4 px progress bar draining,
  slide-out on expiry; pool of 8 slots.

### 7.5 Notifications

`notifications::push({title, message, icon})` by value; the pool owns layout, stacking (top-right,
24 px offset, 8 px gap), animation, and expiry. Module enable/disable, config load, mapping issues,
and perf warnings all surface through this one call site — consistent UX by construction.

### 7.6 HUD (in-game overlay)

Watermark (`woke.wtf 0.1.0 | 240 fps`), right-edge arraylist of enabled modules sorted by name width
with animated insertion/removal, and per-module HUD fragments (cooldown bars, readouts) drawn with
the same component + `render_utils` stack. HUD participates in `needs_render()`.

### 7.7 Render utils (`utils/render_utils`)

Stateless pure functions over `ImDrawList`: `rounded_rect`, `border_stroke`, `shadow_rect`,
`gradient_fill_v/h`, `text_clipped` (ellipsize into `FixedString`), `circle`. The **only** file
permitted to touch `ImDrawList` primitives directly besides ImGui's own backend — keeps the entire
UI visually consistent and re-themeable.

### 7.8 Draw-call suppression

`ModuleManager::needs_render()` — one cached bool updated only on state transitions (GUI toggled,
toast pushed/expired, HUD module enabled/disabled). Swap trampoline:

```
if (!needs_render) return original(...);   // one branch, zero ImGui work
```

When suppressed there is **no** `ImGui::NewFrame`, no vertex generation, no draw-list traversal —
100 % of cycles return to the game. Suppressed state also skips the animation tick (animations are
frozen with the GUI hidden and resume from current values, no snap).

---

## 8. Module Catalog

24 modules across 6 categories. All: `BaseModule` lifecycle, `BaseSetting<T>` settings, game state
via `game_instance` façade only, client-side writes only, visible toggles, toasts on state change.

| Category | Module | Summary | Mode | Settings |
|---|---|---|---|---|
| Combat | Target HUD | Card for the entity under crosshair: name, health, distance. | R | pos mode, scale |
| Combat | Attack Cooldown | Cooldown progress bar under crosshair. | R | color, style |
| Combat | Reach Display | Current client-side reach readout. | R | on/off |
| Combat | Combat Stats | Session counters (swings, hits, cooldown wastes). | R | reset bind |
| Mace | Smash Potential | Fall distance → projected mace smash damage readout. | R | threshold color |
| Mace | Wind Charge CD | Wind charge cooldown HUD chip. | R | position |
| Mace | Mace Stats | Session mace hit/smash counters. | R | reset bind |
| Mace | Smash Flash | Local screen-edge flash when smash conditions are met. | R | color, intensity |
| Misc | ClickGUI | Open/close the GUI (default `RSHIFT`). | N | bind, hold mode |
| Misc | Config Hotkeys | Binds for config load/save slots. | N | 3 binds |
| Misc | Friend Manager | Local nametag color overrides for friends. | R | list editor, color |
| Misc | Client Sound | Local UI click/success sounds via `PlaySound`. | N | volume, toggles |
| Misc | Panic | One bind disables every module. | N | bind |
| Movement | Auto Sprint | Visible auto-sprint toggle (client-side sprint flag). | W | hold/press mode |
| Movement | Safe Walk | Client-side edge-walk assist. | W | on/off |
| Movement | Velocity Display | Knockback velocity readout chip. | R | format |
| Spear | Riptide Indicator | Riptide availability chip. | R | position |
| Spear | Trident CD | Trident/riptide cooldown bar. | R | color |
| Spear | Loyalty HUD | Loyalty return-trip readout. | R | position |
| Visual | Fullbright | Gamma option write for full brightness. | W | slider |
| Visual | HUD | Watermark + arraylist. | N | watermark, order |
| Visual | Zoom | Smooth FOV lerp on bind. | W | factor, smoothness |
| Visual | Trajectories | Client-side projectile path prediction overlay (pure math). | R | color, physics |
| Visual | Custom Crosshair | Local crosshair overlay. | N | style, color, size |

Category badge counts: Combat 4 · Mace 4 · Misc 5 · Movement 3 · Spear 3 · Visual 5 = **24**.

---

## 9. Priority-Ordered Implementation Roadmap

One file at a time, user confirmation after each file (the client's stated protocol), one PR per
step. Each gate must pass before the next step starts.

| # | Step | Files | Gate |
|---|---|---|---|
| 0 | **Repo scaffold** | `.gitignore`, branches, root CMake, `cmake/CompilerWarnings.cmake`, vendored deps, CI YAML, empty `dllmain` | Empty DLL builds Release in CI, loads/unloads cleanly |
| 1 | **Core foundation** | `dllmain`, `core/lifecycle`, `core/logger`, `core/version`, `utils/win32_utils`, `utils/string_buffer` | Colored console + timestamped session file + `latest.log` written on attach |
| 2 | **JVM bridge** | `jni/jni_context`, `jni/mappings`, `jni/reflection_cache`, `jni/game_instance`, `jni/game_types` | `mappings: resolved N/M/K` logged; live `class_310` instance cached |
| 3 | **Hook engine** | `hooks/hook_manager`, `hooks/swap_hook`, `hooks/wndproc_hook`, `hooks/game_thread`, `core/event_bus`, `core/events` | `FrameEvent` fires per swap; keybind posts; clean unhook on unload |
| 4 | **ImGui + macOS chrome** | `ui/theme`, `ui/gui`, `ui/components/traffic_lights`, `ui/animation/*` | Chrome renders <0.2 ms; spring open/close; closes to zero draw calls |
| 5 | **Module system** | `modules/category`, `modules/base_module`, `modules/module_manager`, `settings/*`, `core/config` | Two dummy modules toggle, persist across reinjection |
| 6 | **Widget library** | `ui/components/{pill_toggle, module_card, sidebar, search_bar, keybind_badge}`, `ui/notifications`, `utils/render_utils`, `utils/math_utils` | All components demoed; toggles animate; toasts slide in/out |
| 7 | **Visual modules** | `visual/{fullbright, hud, zoom, custom_crosshair, trajectories}` | First real utility in-game; zero-alloc soak clean |
| 8 | **Movement → Misc → Mace → Spear → Combat** | Remaining module files | Sidebar badges populate with real counts |
| 9 | **Perf + hardening** | Instrumentation, hot-unload audit, error-path audit | Sustained <0.5 ms overlay; unload leaves process stable |
| 10 | **Polish + release** | Theme pass, README rewrite, `v0.1.0` tag | CI artifact green; tagged release |

---

## 10. Development Workflow

### 10.1 Per-file build protocol (this project's stated loop)

1. Architect states the file's purpose + its blueprint section (this document).
2. File is written completely — no stubs, no TODOs, no truncation.
3. Verification: CI build + host tests (sandbox cannot run MSVC — CI is the compile gate).
4. Owner confirms → next file. No file starts before the previous one is confirmed.

### 10.2 Branch & PR flow

- `feature/<roadmap-step>` → `develop` (squash). `develop` → `main` (release PR, tag).
- PR description: files, blueprint section refs, gate evidence (CI link + QA notes).
- CI must be green (build + `/WX` warnings-as-errors + host tests) before merge.

### 10.3 Commit conventions

Conventional Commits, imperative mood, ≤72 char subject:

```
feat(core): add session-logging engine with latest.log mirror
perf(ui): suppress draw calls when clickgui is closed
fix(jni): release global refs in reverse resolution order
```

### 10.4 Runtime artifact layout

```
<game-dir>/woke.dll                  # injected artifact
<mappings.json location>:            # process CWD or exe dir (win32_utils resolves)
/logs/2026-09-20_15-41-00.log        # session log (append)
/logs/latest.log                     # live mirror (truncate each session)
/configs/default.json                # settings persistence
```

---

## 11. Code Review & Safety Checklist

Every PR checks **all** of:

- [ ] No `new`/`malloc`/container growth/`std::string` in `on_tick`/`on_render` paths.
- [ ] `FindClass`/`GetMethodID`/`GetFieldID` appear **only** in `reflection_cache.cpp`.
- [ ] JNI locals bounded by `ScopedLocalFrame`; global refs owned only by `game_instance`/`reflection_cache`.
- [ ] **No packet synthesis/mutation, no network traffic, no chat manipulation** — grep gate:
      `ClientConnection|sendPacket|PacketByteBufs|networkHandler` must not appear in module code.
- [ ] No detection-evasion, obfuscation-for-avoidance, or hide-from-server logic anywhere.
- [ ] Every module state change is user-visible: HUD arraylist entry + toast.
- [ ] All user-facing text/colors from `ui/theme` tokens — no raw hex in widgets.
- [ ] Files ≤ 200 lines; single responsibility; comments explain *why*.
- [ ] MinHook/WndProc/ImGui teardown paths exercised (hot-unload safe).
- [ ] `/W4` clean; Release build green; host tests pass.

---

## 12. Testing & Verification Strategy

### 12.1 Host tests (CI, no Minecraft required — `tests/` target)

- `test_easing` — easing/lerp/spring invariants: monotonicity, endpoints, dt-independence
  (identical accumulated progress at 60/240 Hz), color lerp bounds.
- `test_config_roundtrip` — every setting type → JSON → identical settings; unknown-key and
  missing-key tolerance; schema-version forward compat.
- `test_mappings_parser` — §5.4 schema variants (1)–(4); missing file, malformed JSON, unknown
  keys → `WARN` + invalid handles, never exceptions/crashes.

### 12.2 In-game manual QA (per roadmap gate)

| Gate | Checklist |
|---|---|
| Step 0 | Inject into 1.21.11 Fabric singleplayer; console appears; unload restores process |
| Step 1 | Log colors correct; timestamp uses local timezone; `latest.log` mirrors session file |
| Step 2 | Boot logs `resolved N classes / M methods / K fields`; cached player handle valid in-world |
| Step 3 | Toggle a bind → toast + arraylist; key pressed while GUI open is consumed by GUI |
| Step 4 | GUI spring open/close; red light hides; frame time <0.2 ms with chrome open |
| Step 5 | Toggle dummy module → persists across reinject via `configs/default.json` |
| Step 6 | Pill knob + color cross-fade together; chevron eases; toasts stack ≤ 8 |
| Step 7 | Fullbright/HUD/Zoom/Trajectories behave in singleplayer; zero-alloc soak 10 min |
| Step 8 | All 24 modules live; badge counts correct; `panic` disables everything |
| Step 9 | 10-min soak: overlay avg <0.5 ms; inject/eject ×20 stable; no local-ref leaks (log line) |
| Step 10 | Tag `v0.1.0`; README + release notes; CI artifact attached |

---

## 13. Risk Register & Error-Handling Matrix

| # | Risk | Likelihood | Mitigation (architectural) |
|---|---|---|---|
| R-01 | Loader lock deadlock on attach | Med | Worker-thread-only boot; no JNI/static-init work in `DLL_PROCESS_ATTACH` |
| R-02 | GLFW `HWND` resolution fails | Med | `glfwGetWin32Window` primary, EnumWindows fallback; boot degrades to console-only, `ERROR` logged |
| R-03 | `mappings.json` missing/stale | High (absent today) | Defensive loader (§5.4); modules auto-disable; GUI/config still work; single-file repair point |
| R-04 | Game update changes intermediary IDs | Med | IDs live only in `mappings.json`; Yarn 1.21.11 branch is truth; no hard-coded IDs in code |
| R-05 | Concurrent modification crash | Low | Single-threaded JNI access on swap hook (§6.2); no Java-side dispatch needed |
| R-06 | Local-ref leak over long session | Med | `ScopedLocalFrame` per frame; soak test counts refs via `PushLocalFrame` audit (Step 9) |
| R-07 | Unload while hooks active | Med | Teardown order §4.9; deferred hook removal §4.5; ×20 inject/eject gate |
| R-08 | Overlay exceeds 0.5 ms on iGPU | Low | Suppression branch; per-component budgets §6.4; perf meter auto-warns |
| R-09 | CRT/state issues inside JVM process | Low | `/MD` runtime, no CRT objects cross our boundary; `/MT` documented as fallback (D-05) |
| R-10 | Keybind conflicts with game keys | Med | GUI-open suppression; `consumed` flag; NumLock/ScrollLock capture at boot (§4.7) |

---

## 14. Decision Log

| # | Decision | Rationale |
|---|---|---|
| D-01 | **DECIDED:** ship the optional `launcher/` injector in-repo (`WOKE_BUILD_INJECTOR`, default ON) | Self-contained local testing; a standard `LoadLibraryW` loader is not an evasion tool and keeps the repo reproducible without third-party injectors |
| D-02 | **DECIDED:** Movement ships exactly Auto Sprint, Safe Walk, Velocity Display (3 modules) | Minimal visible QoL set; any additional movement behavior is a separate, explicitly reviewed decision |
| D-03 | OpenGL (`wglSwapBuffers`) primary; DXGI behind flag | 1.21.11 + LWJGL renders OpenGL; DXGI kept for contingency |
| D-04 | Swap hook *is* the main-thread dispatcher (no `mc.execute()` bridge) | Structural thread-safety; removes cross-thread handoff entirely |
| D-05 | `/MD` CRT default; `/MT` documented fallback | Minimal surprises inside the JVM process |
| D-06 | ImGui docking branch, Win32+OpenGL3 backends only | Binary size, single-window chrome ownership |
| D-07 | 24-module catalog (§8) — categories fixed by spec, counts final | Sidebar badges and roadmap gates depend on exact list |
| D-08 | `mappings.json` treated as runtime asset, schema fixed (§5.4) | Absent in workspace; defensive loader keeps boot alive without it |

---

**End of blueprint.** Implementation is underway:

| Step | State | Evidence |
|---|---|---|
| 0 — repo scaffold | landed | CI builds the empty DLL Release in `windows-latest`, host tests run on `ubuntu-latest` |
| 1 — core foundation | landed | logger writes the colored console line, the timestamped session file and `latest.log`; `dllmain` boots from one worker thread |
| 2 — JVM bridge | landed | `mappings: resolved 39 classes / 2726 methods / 1251 fields` with all 7 anchors verified; live `class_310` instance cached by `game_instance`; host tests parse the shipped asset and every schema variant |

D-01 and D-02 are decided (see above). Design notes worth carrying forward:

- **`src/jni/mappings.cpp` is portable on purpose.** No `jni.h`, no `windows.h`, no logger: it
  collects warnings as strings and the caller reports them. That is what lets the host test suite
  parse the real `mappings.json` and assert the anchor table on Linux, in seconds, with no game
  running — the mapping asset is the component a game update breaks first, so it is the component
  that must be cheapest to verify.
- **The JNI bridge is compile-gated.** `cmake/JavaHeaders.cmake` finds `jni.h` (explicit override →
  `JAVA_HOME` → usual install roots). CI installs JDK 21 and configures `WOKE_REQUIRE_JNI=ON`, so a
  missing JDK is a hard configuration error there: a green build proves the bridge compiled instead
  of proving it was skipped.
- **Handles are cached inside the registry entries** as opaque `void*` slots, so a per-tick read is a
  hash hit plus a pointer read — no `std::string` is constructed and no map is grown after boot.
- **Late client binding.** Injecting before the game creates its client instance is normal, so a
  missing instance is a warning plus a bounded retry from the worker loop (30 s at the 10 Hz
  cadence), never a boot failure.

Next gate: Step 3 (hook engine), which moves the frame pipeline onto the game thread via the swap
hook so JNI access becomes inherently serialized with the game (§6.2).
