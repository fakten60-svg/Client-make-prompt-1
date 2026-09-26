# woke.wtf

A native C++ injection utility client for **Minecraft 1.21.11 (Fabric)** on **x64 Windows**
(`javaw.exe`) — built for private-server testing and local singleplayer development.

`woke.dll` statically links Dear ImGui (v1.92.9b-docking), MinHook (v1.3.4) and nlohmann/json
(v3.12.0). It hooks the game's swap chain for its frame loop, draws the macOS-styled ClickGUI
overlay (default bind `RSHIFT`) and exposes 24 modules across Combat, Mace, Misc, Movement,
Spear and Visual — readouts of client-side state and writes the client is allowed to make to
itself (gamma, FOV, holding the game's own sprint/sneak keys). The full blueprint, module
catalogue and testing strategy live in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md); §0 of
that document is the live status board.

## Scope guardrails (read first)

- **Private servers and local singleplayer only.** Never public multiplayer servers.
- **EULA-compliant**, educational/local use.
- **Client-side reads and writes only:** no packet synthesis, no network traffic, no chat
  manipulation, no detection-evasion logic — enforced mechanically by a grep gate in CI and by
  the module-source scan in the host tests (§11 of the blueprint).
- Every module state change is user-visible: HUD arraylist entry + toast.

## Building (Windows, x64 Release)

Requirements: CMake ≥ 3.24, Visual Studio 2022 or newer with the C++ toolset, and a JDK 21
(Temurin works) so `jni.h` is available.

```sh
cmake -B build -A x64 -DCMAKE_BUILD_TYPE=Release -DWOKE_WARNINGS_AS_ERRORS=ON -DWOKE_REQUIRE_JNI=ON
cmake --build build --config Release --parallel
```

Artifacts land in `bin/Release/`: `woke.dll` and `woke_injector.exe`. The third-party
dependencies are fetched at configure time from pinned upstream tags (`cmake/ThirdParty.cmake`).

- `-DWOKE_REQUIRE_JNI=ON` makes a missing JDK a configure error instead of silently compiling
  the JVM bridge out. Without a JDK the DLL still builds and boots, but cannot read game state.
- Every CI pass builds exactly this (`.github/workflows/ci.yml`), verifies per-subsystem
  markers inside the binary, and uploads the `woke-windows-x64` artifact
  (`woke.dll` + `woke_injector.exe`).

## Host tests (no Minecraft required)

CI runs the portable suite on Linux on every push. Locally with CMake:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DWOKE_BUILD_TESTS=ON -DWOKE_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

On a host with a C++20 compiler but no CMake:

```sh
tools/build_host_tests.sh
```

The script replicates the CMake target's TU list, include paths and defines by hand; its
header documents the two dependency checkouts it expects (`WOKE_IMGUI_DIR` / `WOKE_JSON_DIR`
override the locations).

## Usage

1. Start Minecraft **1.21.11 Fabric** and get into a world.
2. Keep `mappings.json` reachable — it is resolved from the process CWD or the exe directory.
   Every JNI handle the client uses is looked up from this anchor table; a stale one degrades
   to a reduced `mappings: resolved N/M/K` tally in the log rather than a crash.
3. Inject:

   ```sh
   woke_injector <path\to\woke.dll>                # default target: javaw.exe
   woke_injector <path\to\woke.dll> --pid 12345    # or --process javaw.exe
   ```

4. Press **RSHIFT** for the ClickGUI. Artifacts the client writes land in the game directory:
   `logs/<timestamp>.log` with a live `logs/latest.log` mirror, and settings persistence in
   `configs/default.json`. The Panic bind disables every module; unloading restores the process.

After a game update, regenerate the mapping asset from the new Yarn build instead of editing it
by hand — the parser test fails CI when the asset no longer matches the anchors the code uses:

```sh
python3 tools/generate_mappings.py --jar yarn-1.21.11+build.6-v2.jar \
    --version 1.21.11 --source net.fabricmc:yarn:1.21.11+build.6 --out mappings.json
```

## Release

`v0.1.0` is tagged on `main`. CI attaches the Release DLL (`woke.dll`) and `woke_injector.exe`
to the GitHub release; every build also keeps them available as the `woke-windows-x64` artifact.
