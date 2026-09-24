#pragma once

// wglSwapBuffers trampoline (blueprint §4.6).
//
// Minecraft 1.21.11 renders through LWJGL/OpenGL, so the swap call is the frame boundary.
// Hooking it buys three things at once: a per-frame entry point, a point that runs on the
// game thread (which is what makes every JNI read inherently serialized with the game, D-04),
// and a place where a queued hook removal can be executed safely.
//
// Windows-only.

#ifndef _WIN32
#error "swap_hook.h is Windows-only; portable translation units must not include it."
#endif

#include <cstdint>

namespace woke::hooks {

// Resolves wglSwapBuffers from opengl32.dll and hooks it. Returns false - with an ERROR
// logged - when the export cannot be found, which degrades the client to console-only rather
// than crashing the host.
bool install_swap_hook() noexcept;

// Disables and removes the hook. Idempotent.
void remove_swap_hook() noexcept;

[[nodiscard]] bool swap_hook_installed() noexcept;

// Frames observed since injection. The in-game step-3 gate reads this: a growing count means
// the trampoline is live inside the game.
[[nodiscard]] std::uint64_t swap_hook_frame_count() noexcept;

} // namespace woke::hooks
