# In-Game QA Protocol — woke.wtf v0.1.0

**Status:** the automated gates (MSVC x64 Release build, 1401 portable host checks, ASan+UBSan,
GCC `-fanalyzer`, clang-tidy, artifact markers) all pass in CI. What none of them can prove is
that `woke.dll` functions against a live Minecraft 1.21.11 Fabric JVM with a real OpenGL
context. This file is the protocol for the human who can. Work top to bottom; record PASS/FAIL
plus evidence for every line, and stop at the first crash.

On any failure: capture `logs/latest.log` (the whole file, not a excerpt), a screenshot of what
was on screen, and note which module was enabled. Attach all three to the issue.

---

## 1. Prerequisites

- Windows 10 or 11, x64. An x64 OS and an x64 JVM are hard requirements (the injector refuses
  32-bit targets, and the DLL is x64-only).
- Minecraft **1.21.11** with **Fabric Loader** installed and working. The mapping asset is
  generated for exactly this game version; the client refuses a mismatched one at boot.
- Java 21 (Temurin works) as the JVM that runs the game.
- A clean single-player world (creative or survival — the readout modules do not care).
- Administrator rights are **not** required; the injector runs as the same user as the game.

## 2. Download the release artifacts

1. Open the `v0.1.0` GitHub Release page for this repository.
2. Download **`woke.dll`** and **`woke_injector.exe`**.
3. Optional but recommended: download `mappings.json` from the repository root (same commit as
   the release tag) — see §3.

## 3. Setup

```
<some folder>\
    woke.dll
    woke_injector.exe
    mappings.json        <- from the repo root; colocate it here
```

- `mappings.json` is resolved from the process CWD or the DLL's directory. Keeping it next to
  `woke.dll` and launching the injector from that folder covers both.
- Every JNI handle the client uses is looked up through this file. A stale or missing one is
  not a crash: the client boots, the GUI works, and modules that need game state report
  unavailable. The boot log line `mappings: resolved N classes / ...` tells you what it got.

## 4. Start the game

1. Launch Minecraft 1.21.11 (Fabric profile) — the plain launcher is fine, no JVM flags needed.
2. Create or open a single-player world.
3. Note the window title / that the game is fully loaded before injecting.

## 5. Inject

```
woke_injector.exe <path\to\woke.dll>
```

- Default target process is `javaw.exe`. Alternatives: `--pid 12345` or
  `--process javaw.exe`.
- Expected console output: `loaded '<path>' into pid <pid> (module 0x...)` followed by
  `check the game's logs/ directory for the session file and latest.log`.
- A `LoadLibraryW returned NULL` line means the DLL was rejected — capture the output and stop.

## 6. Verification checklist

Mark each line PASS/FAIL with evidence (log excerpt, screenshot, or "n/a").

### 6.1 Boot

- [ ] `logs/latest.log` exists in the **game directory** (not the injector's folder) and starts
      with a boot header (`woke.wtf 0.1.0 | Minecraft 1.21.11 Fabric | ...`).
- [ ] The boot log contains one `boot: <step> ready (...)` line per successful step, and
      **either** all twelve steps ready **or** an explicit `degraded mode` warning for the
      skipped ones. A missing/failed `mappings` or `jvm-bridge` step is expected only if you
      deliberately removed `mappings.json`.
- [ ] `mappings: resolved 39 classes / ... methods / ... fields` with a following
      `mappings: 7/7 anchor identifiers verified` line.
- [ ] **No crash within 60 seconds of injection.** The game stays responsive, world visible.

### 6.2 GUI

- [ ] **RSHIFT opens the ClickGUI overlay.** The game underneath keeps running.
- [ ] **Traffic-light buttons respond to click** (red closes the GUI).
- [ ] **Search field filters module cards as you type.**
- [ ] Toggling a module animates its pill and pushes a toast.
- [ ] **Modules tab shows all 6 categories** with badge counts (Combat 4 · Mace 4 · Misc 5 ·
      Movement 3 · Spear 3 · Visual 5 = 24).
- [ ] ESC / the toggle key closes the GUI; the game receives mouse + keyboard again
      (camera turns, hotbar scrolls).

### 6.3 Modules (one toggle each; game state visible where noted)

- [ ] **Fullbright** → gamma visually changes while enabled, returns on disable (day/night does
      not matter; toggle it twice).
- [ ] **Zoom** (hold `C` by default) → FOV narrows smoothly, restores on release.
- [ ] **HUD** → watermark + module arraylist appear top-left/top-right as configured.
- [ ] **Trajectories** → hold a bow, draw it back: an arc path renders ahead of the crosshair.
- [ ] **Custom Crosshair** → the vanilla crosshair is replaced by the configured style; cycle
      the 4 shapes.
- [ ] **Auto Sprint** → walking forward sprints without double-tapping W; stops when disabled.
- [ ] **Wind Charge CD** → hold/throw a wind charge: the chip shows the game's own cooldown
      counting down, hides when ready (per its setting).
- [ ] **Safe Walk** → walking off an edge stops at the lip while enabled.
- [ ] Toggle **every one of the 24 modules** at least once. **No crash, no stuck state** —
      after the sweep, Panic (its bind) must disable everything, and the arraylist must empty.

### 6.4 Soak and stability

- [ ] **Wait 10 minutes idle** (GUI closed, world running). The session log contains `soak:`
      lines at 60 s intervals; the averages stay inside `chrome < 0.2 ms / pipeline < 0.5 ms`
      (a `frame-pipeline: budget exceeded` warning is a FAIL evidence item).
- [ ] **Inject/eject cycle ×20**: unload (see §7) and re-inject the DLL twenty times. The
      process must remain stable through all twenty, with no growing lag between cycles.
- [ ] After the final eject, the final `teardown AUDIT:` lines report **no findings** and the
      process keeps playing normally.

## 7. Unloading the DLL

The client unloads on request rather than on process exit so hot-unload stays a tested path:

- Injected copies register their unload trigger at boot; use the same mechanism your session
  used (the console log line `shutdown: unloading woke.wtf ...` is the evidence the unload
  path actually ran, versus the process just terminating).
- Expected sequence in the log: `shutdown: unloading ...` → per-step reverse teardown →
  `teardown AUDIT:` lines with no findings → nothing further.
- The game must remain playable after unload (input, rendering, saving the world).

## 8. Failure reporting format

| Item | Result | Evidence |
|---|---|---|
| 6.1 latest.log boot header | PASS | log line "..." |
| 6.3 Fullbright toggle | FAIL | gamma unchanged, log shows `game: ... refused` |

Attach: `logs/latest.log` (full), screenshot(s), the injector's console output if relevant.

## 9. What this protocol cannot test

Anything requiring a hostile or multi-player environment is out of scope by design — this
client is for private-server testing and local singleplayer only. There is deliberately no
verification item that involves packet traffic: §11 of the blueprint forbids it, CI greps for
it, and the QA report is expected to state the same.
