#pragma once

// ClickGUI (blueprint §7.1, §7.8) - roadmap step 4: Dear ImGui + macOS chrome.
//
// Three separate responsibilities, deliberately kept in one file because they are one lifecycle:
//
//   * `initialize()`  creates the ImGui context, applies the theme and subscribes the GUI's own
//                     input handlers. It touches no window and no GL context, so it is safe to run
//                     from the boot worker thread.
//   * `render_overlay()` is the entire per-frame path and runs on the game thread inside the swap
//                     trampoline, where the game's GL context is current. It is also where the
//                     §7.8 suppression contract lives: while the chrome is hidden and its close
//                     animation has finished, this returns before ImGui::NewFrame - no frame, no
//                     vertex generation, no draw-list traversal.
//   * `handle_window_message()` feeds the ImGui Win32 backend from the subclassed window procedure
//                     and reports whether the GUI consumed the message.
//
// Windows-only: it composes the overlay against the game's HWND and its OpenGL context.

#ifndef _WIN32
#error "gui.h is Windows-only; portable translation units must not include it."
#endif

#include <cstddef>
#include <cstdint>

namespace woke::ui {

// Creates the ImGui context, applies the theme tokens and subscribes KeyEvent/FocusEvent.
// Idempotent. Returns false only when the ImGui context could not be created, in which case the
// overlay stays disabled and the game is unaffected.
bool initialize() noexcept;

// Releases the renderer backend, the ImGui context and every animation slot. Safe to call after a
// partial initialisation.
void shutdown() noexcept;

// Records the game window for the lazy renderer initialisation. Called by the boot sequence after
// the WndProc hook has resolved it; nullptr clears it. The backends are deliberately not
// initialised here: ImGui_ImplOpenGL3_Init needs a current GL context, which only the first swap
// detour call can guarantee.
void attach_window(void* hwnd) noexcept;

// Offers one window message to the ImGui Win32 backend. Returns true when the GUI consumed it and
// the caller must not forward it to the game.
bool handle_window_message(
    void* hwnd, unsigned int message, unsigned long long wparam, long long lparam) noexcept;

// The per-frame overlay path. No-op unless the chrome is visible (or still animating closed) and
// the backend is live. Runs on the game thread with the GL context current.
void render_overlay() noexcept;

[[nodiscard]] bool visible() noexcept;
void set_visible(bool visible) noexcept;
void toggle() noexcept;

// False once the window is hidden *and* its close animation has settled - the condition the swap
// trampoline's suppression branch reports on.
[[nodiscard]] bool needs_render() noexcept;

// The Misc/ClickGUI bind (blueprint §8: default RSHIFT). Only the key-down edge toggles.
[[nodiscard]] int toggle_key() noexcept;
void set_toggle_key(int virtual_key) noexcept;

// Saves the current module state to configs/default.json. Called after any toggle through the
// GUI or a keybind; also used at shutdown. Returns false when nothing was saved.
[[nodiscard]] bool save_config() noexcept;

// The module registry's per-frame fan-out lives in the frame scheduler; these hooks keep the
// GUI's own bookkeeping (card hover states, enabled badges) in step with the registry.
[[nodiscard]] std::size_t registered_module_count() noexcept;

// ── Cross-system notifications (roadmap step 6) ───────────────────────────────────
// The toast pool belongs to the overlay, but not every state change originates in it: the module
// keybind dispatcher is a boot-sequence subscriber. These are the entry points it calls, so a
// keybind toggle is as visible as a ClickGUI click (§4.4). Both are no-ops before initialize().

// Reports whatever module(s) `virtual_key` just toggled. The GUI resolves the names and pushes
// one toast per module, which keeps the toast text in the layer that owns the toast pool.
void notify_keybind_toggle(int virtual_key) noexcept;

// A generic toast. `warning` selects the yellow accent.
void notify(const char* title, const char* message, bool warning = false) noexcept;

struct Stats {
    bool initialized = false;
    bool renderer_ready = false;
    bool visible = false;
    bool collapsed = false;
    bool suppressed = false; // hidden and settled: frames cost nothing
    std::size_t frames_rendered = 0;
    std::size_t suppressed_frames = 0;
    float open_amount = 0.0f;
    float last_chrome_ms = 0.0f;
    float average_chrome_ms = 0.0f;
    float worst_chrome_ms = 0.0f;
    std::size_t animation_slots = 0;
    std::size_t animation_overflows = 0;
};

[[nodiscard]] Stats stats() noexcept;

} // namespace woke::ui
