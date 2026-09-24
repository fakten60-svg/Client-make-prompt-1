#pragma once

// Window procedure subclass (blueprint §4.7).
//
// GLFW owns the game window's message loop, so the only way to see raw input before the game
// does is to subclass the window procedure. Everything this file produces is an event on the
// bus; a handler that consumes an input (the ClickGUI taking a key) makes the message be
// swallowed instead of reaching the game, which is what makes "the GUI key must not also
// toggle a module" work without either system knowing about the other.
//
// Windows-only, and deliberately free of windows.h in the header: the HWND is exposed as an
// opaque handle so callers do not inherit the Win32 include order.

#ifndef _WIN32
#error "wndproc_hook.h is Windows-only; portable translation units must not include it."
#endif

namespace woke::hooks {

// Finds the GLFW window and subclasses its procedure. Returns false - with an ERROR logged -
// when no window can be identified, which is the documented degraded mode (R-02).
bool install_wndproc_hook() noexcept;

// Restores the original window procedure. Idempotent, and safe to call after the window has
// already been destroyed (the subclass then died with the window).
void remove_wndproc_hook() noexcept;

[[nodiscard]] bool wndproc_hook_installed() noexcept;

// The subclassed window as an opaque HWND.
[[nodiscard]] void* window_handle() noexcept;

// Lock-key state captured at install time and kept up to date from the message stream.
//
// GetAsyncKeyState reports the *toggle* state of the lock keys, and that read is only
// reliable when the calling thread owns the foreground window - so the client records the
// state once at boot and tracks the WM_KEYUP of each lock key instead of polling.
[[nodiscard]] bool num_lock_state() noexcept;
[[nodiscard]] bool scroll_lock_state() noexcept;
[[nodiscard]] bool caps_lock_state() noexcept;

// Messages routed to the event bus since install. A non-zero, growing count is the step-3
// evidence that the subclass is receiving input.
[[nodiscard]] unsigned long long routed_message_count() noexcept;

} // namespace woke::hooks
