#pragma once

// Frame scheduler (blueprint §4.8).
//
// The swap trampoline calls on_frame() once per frame, on the game thread. Everything that
// happens per frame or per tick is ordered here, in one function, so the ordering is reviewable
// rather than implied by the call sites:
//
//   1. advance the frame clock and clamp the delta,
//   2. post FrameEvent,
//   3. gate at 20 Hz and post TickEvent,
//   4. update the frame-rate window,
//   5. watch world/player transitions,
//   6. measure the pipeline and warn if the overlay budget is exceeded.
//
// Step 4's animation controller and step 5's module fan-out insert themselves as event
// subscribers; they do not get to reorder this function.
//
// Windows-only, and the only file in the hook layer that may read game state.

#ifndef _WIN32
#error "game_thread.h is Windows-only; portable translation units must not include it."
#endif

#include <cstddef>
#include <cstdint>

namespace woke::hooks::game_thread {

// Resets timing state and subscribes the built-in frame observer. Idempotent.
void start() noexcept;

// Unsubscribes and stops scheduling. Frame work becomes a no-op afterwards.
void stop() noexcept;

// Called once per swap, on the game thread.
void on_frame() noexcept;

[[nodiscard]] bool running() noexcept;

// False when nothing is subscribed and no overlay is requested. This is the predicate the
// draw-call suppression contract hangs off (§7.8): with it false, the frame returns after one
// clock read and one comparison.
[[nodiscard]] bool needs_frame_work() noexcept;

// Step 4 turns this on while the ClickGUI is visible. Until then the pipeline runs only for
// subscribers, which is why the built-in observer exists.
[[nodiscard]] bool overlay_requested() noexcept;
void set_overlay_requested(bool requested) noexcept;

struct Stats {
    std::uint64_t frames = 0;
    std::uint64_t ticks = 0;
    float frames_per_second = 0.0f;
    float average_pipeline_ms = 0.0f;
    float worst_pipeline_ms = 0.0f;
    std::size_t perf_warnings = 0;
};

[[nodiscard]] Stats stats() noexcept;

} // namespace woke::hooks::game_thread
