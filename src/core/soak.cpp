#include "core/soak.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "core/build_config.h"
#include "core/event_bus.h"
#include "core/events.h"
#include "core/logger.h"
#include "core/perf.h"
#include "hooks/game_thread.h"
#include "ui/gui.h"

#if WOKE_HAVE_JNI
#include "jni/jni_context.h"
#endif

// The soak reporter's implementation. The interval snapshot is composed from the meters the
// render paths already maintain - no second copy of any number - and the pass/fail judgement
// is perf::soak_interval_ok(), the same policy the host suite asserts.

namespace woke::perf {
namespace {

constexpr const char* kSoakMarker = "soak";

struct SoakState {
    bool running = false;
    double elapsed_seconds = 0.0;

    std::uint64_t frames_at_start = 0;
    std::uint64_t suppressed_at_start = 0;
    std::uint64_t local_push_failures_at_start = 0;
    std::uint64_t local_pop_failures_at_start = 0;

    events::Subscription tick_observer{};
    bool ever_reported = false;
};

SoakState g_soak{};

std::uint64_t chrome_frames() noexcept {
    return ui::stats().frames_rendered;
}

std::uint64_t suppressed_frames() noexcept {
    return ui::stats().suppressed_frames;
}

std::uint64_t push_failures() noexcept {
#if WOKE_HAVE_JNI
    return jni::local_frame_stats().push_failed;
#else
    return 0;
#endif
}

std::uint64_t pop_failures() noexcept {
#if WOKE_HAVE_JNI
    return jni::local_frame_stats().pop_failed;
#else
    return 0;
#endif
}

// Deliberately not noexcept: it is passed as a non-type template argument to the bus, and a
// 'pointer to noexcept function' argument for a plain function-pointer parameter is a
// conformance area worth not depending on.
void on_tick(events::TickEvent& tick) {
    if (!g_soak.running) {
        return;
    }
    g_soak.elapsed_seconds += static_cast<double>(tick.delta_seconds);
    if (g_soak.elapsed_seconds < kSoakIntervalSeconds) {
        return;
    }
    g_soak.elapsed_seconds = 0.0;
    report_soak_now();
}

} // namespace

void start_soak() noexcept {
    if (g_soak.running) {
        return;
    }
    g_soak = SoakState{};
    g_soak.frames_at_start = chrome_frames();
    g_soak.suppressed_at_start = suppressed_frames();
    g_soak.local_push_failures_at_start = push_failures();
    g_soak.local_pop_failures_at_start = pop_failures();
    g_soak.tick_observer = events::bus().subscribe<events::TickEvent, &on_tick>();
    g_soak.running = g_soak.tick_observer.valid();
    if (!g_soak.running) {
        WOKE_LOG_WARN("soak: the tick observer could not subscribe - no soak reporting");
    }
}

SoakInterval soak_snapshot() noexcept {
    const ui::Stats gui = ui::stats();
    const hooks::game_thread::Stats pipeline = hooks::game_thread::stats();

    SoakInterval interval;
    interval.average_chrome_ms = gui.average_chrome_ms;
    interval.worst_chrome_ms = gui.worst_chrome_ms;
    interval.average_pipeline_ms = pipeline.average_pipeline_ms;
    interval.worst_pipeline_ms = pipeline.worst_pipeline_ms;
    interval.frames = gui.frames_rendered - g_soak.frames_at_start;
    interval.suppressed = gui.suppressed_frames - g_soak.suppressed_at_start;
    interval.local_push_failures = push_failures() - g_soak.local_push_failures_at_start;
    interval.local_pop_failures = pop_failures() - g_soak.local_pop_failures_at_start;
    interval.ok = soak_interval_ok(interval.average_chrome_ms, interval.average_pipeline_ms);
    return interval;
}

void report_soak_now() noexcept {
    if (!g_soak.running) {
        return;
    }
    const SoakInterval interval = soak_snapshot();

    // The counters are session-failure counts, so a non-zero delta is a finding on its own:
    // it means a JNI local-reference frame was pushed or popped unsuccessfully, which is the
    // leak shape risk R-06 describes.
    if (interval.local_push_failures != 0 || interval.local_pop_failures != 0) {
        WOKE_LOG_WARN(
            "%s: local-ref failures this interval (push %llu, pop %llu) - investigate R-06",
            kSoakMarker, static_cast<unsigned long long>(interval.local_push_failures),
            static_cast<unsigned long long>(interval.local_pop_failures));
    }

    if (interval.ok) {
        WOKE_LOG_INFO(
            "%s: chrome avg %.3f ms (worst %.3f, budget %.2f) | pipeline avg %.3f ms "
            "(worst %.3f, budget %.2f) | %llu frame(s), %llu suppressed, %llu local-ref failure(s)",
            kSoakMarker, static_cast<double>(interval.average_chrome_ms),
            static_cast<double>(interval.worst_chrome_ms), static_cast<double>(kChromeBudgetMs),
            static_cast<double>(interval.average_pipeline_ms),
            static_cast<double>(interval.worst_pipeline_ms),
            static_cast<double>(kPipelineBudgetMs),
            static_cast<unsigned long long>(interval.frames),
            static_cast<unsigned long long>(interval.suppressed),
            static_cast<unsigned long long>(
                interval.local_push_failures + interval.local_pop_failures));
    } else {
        WOKE_LOG_WARN(
            "%s: BUDGET EXCEEDED - chrome avg %.3f ms (budget %.2f) | pipeline avg %.3f ms "
            "(budget %.2f) | %llu frame(s), %llu suppressed, %llu local-ref failure(s)",
            kSoakMarker, static_cast<double>(interval.average_chrome_ms),
            static_cast<double>(kChromeBudgetMs),
            static_cast<double>(interval.average_pipeline_ms),
            static_cast<double>(kPipelineBudgetMs),
            static_cast<unsigned long long>(interval.frames),
            static_cast<unsigned long long>(interval.suppressed),
            static_cast<unsigned long long>(
                interval.local_push_failures + interval.local_pop_failures));
    }
    g_soak.ever_reported = true;
}

void stop_soak() noexcept {
    if (g_soak.running) {
        events::bus().unsubscribe(g_soak.tick_observer);
        g_soak.tick_observer = events::Subscription{};
        g_soak.running = false;
        report_soak_now();
    }
}

bool soak_reported() noexcept {
    return g_soak.ever_reported;
}

} // namespace woke::perf
