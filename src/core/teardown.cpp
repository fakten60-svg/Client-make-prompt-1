#include "core/teardown.h"

#include <windows.h>

#include <cstddef>

#include "core/build_config.h"
#include "core/logger.h"
#include "hooks/game_thread.h"
#include "hooks/hook_manager.h"
#include "hooks/swap_hook.h"
#include "hooks/wndproc_hook.h"
#include "ui/gui.h"

#if WOKE_HAVE_JNI
#include "jni/game_instance.h"
#include "jni/jni_context.h"
#endif

// The audit reads each system's own "am I still doing anything?" accessors after shutdown.
// Every check is a read; nothing here re-enters the system it inspects, and nothing allocates.

namespace woke::perf {
namespace {

constexpr std::size_t kJniAttachReserve = 2; // expected leftover attachments: none, but see below

void record(TeardownFinding* out, std::size_t capacity, std::size_t& count, const char* system,
    const char* detail) noexcept {
    if (count < capacity) {
        out[count].system = system;
        out[count].detail = detail;
    }
    ++count;
}

} // namespace

std::size_t verify_teardown(TeardownFinding* out, std::size_t capacity,
    std::size_t bus_handlers) noexcept {
    std::size_t count = 0;

    if (bus_handlers != 0) {
        record(out, capacity, count, "event-bus",
            "subscriptions remain after unsubscribe - a handler would call into unmapped code");
    }

    if (hooks::active_hook_count() != 0) {
        record(out, capacity, count, "hooks", "MinHook hooks are still installed");
    }
    if (hooks::queued_removal_count() != 0) {
        record(out, capacity, count, "hooks", "deferred hook removals were never drained");
    }
    if (hooks::game_thread::running()) {
        record(out, capacity, count, "frame-scheduler",
            "the scheduler is still running after stop()");
    }
    if (hooks::game_thread::overlay_requested()) {
        record(out, capacity, count, "frame-scheduler",
            "the overlay-requested flag stayed set across shutdown");
    }

    const ui::Stats gui = ui::stats();
    if (gui.initialized) {
        record(out, capacity, count, "overlay", "the ImGui context survived ui::shutdown()");
    }
    if (gui.renderer_ready) {
        record(out, capacity, count, "overlay", "the renderer backend is still initialised");
    }

#if WOKE_HAVE_JNI
    if (game::available()) {
        record(out, capacity, count, "jvm-bridge",
            "a cached game reference survived game::shutdown() - a global ref leak");
    }
    // Threads the JVM attached *itself* (the game thread) are left attached by design
    // (jni_context::detach_current_thread only detaches threads we attached), so the expected
    // residue is bounded, not zero; anything above the reserve is ours and a finding.
    const std::size_t attached = jni::attached_thread_count();
    if (attached > kJniAttachReserve) {
        record(out, capacity, count, "jvm-bridge",
            "more of our threads than expected are still attached to the JVM");
    }
    const jni::LocalFrameStats frames = jni::local_frame_stats();
    if (frames.push_failed != 0 || frames.pop_failed != 0) {
        record(out, capacity, count, "jvm-bridge",
            "local-reference frames failed during the session (see the soak log lines)");
    }
#else
    (void)kJniAttachReserve;
#endif

    for (std::size_t index = 0; index < count && index < capacity; ++index) {
        WOKE_LOG_ERROR("teardown AUDIT: %s - %s", out[index].system, out[index].detail);
    }
    if (count == 0) {
        WOKE_LOG_INFO("teardown AUDIT: clean - no residue in bus, hooks, scheduler, overlay or "
                      "JVM bridge");
    } else {
        WOKE_LOG_ERROR("teardown AUDIT: %zu finding(s) - the process was not left clean", count);
    }
    return count;
}

} // namespace woke::perf
