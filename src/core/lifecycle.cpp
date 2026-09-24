#include "core/lifecycle.h"

#include <atomic>

#include "core/logger.h"
#include "core/version.h"

namespace woke::lifecycle {
namespace {

constexpr DWORD kWorkerIdleSleepMs = 100;  // ~10 Hz flush cadence

std::atomic<bool> g_unload_requested{false};
bool g_boot_ok = false;

using StepFn = bool (*)() noexcept;

struct Step {
    const char* name;
    StepFn run;
};

// ── Boot steps ───────────────────────────────────────────────────────────────────
// Order is fixed and owned here. Later roadmap steps append their stage to this table
// (mappings -> JNI -> hooks -> theme -> modules -> config) instead of self-initialising
// anywhere else, which is what keeps boot and hot-unload symmetric.

bool start_logger() noexcept {
    return woke::logger::init();
}

constexpr Step kBootSteps[] = {
    {"logger", &start_logger},
};

double elapsed_ms(const LARGE_INTEGER& start, const LARGE_INTEGER& end, const LARGE_INTEGER& frequency) {
    if (frequency.QuadPart == 0) {
        return 0.0;
    }
    const double ticks = static_cast<double>(end.QuadPart - start.QuadPart);
    return (ticks * 1000.0) / static_cast<double>(frequency.QuadPart);
}

} // namespace

void boot(HMODULE self) noexcept {
    LARGE_INTEGER frequency{};
    (void)::QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER boot_start{};
    (void)::QueryPerformanceCounter(&boot_start);

    bool all_steps_succeeded = true;
    for (const Step& step : kBootSteps) {
        LARGE_INTEGER step_start{};
        (void)::QueryPerformanceCounter(&step_start);

        const bool succeeded = step.run();

        LARGE_INTEGER step_end{};
        (void)::QueryPerformanceCounter(&step_end);
        const double step_ms = elapsed_ms(step_start, step_end, frequency);

        if (succeeded) {
            WOKE_LOG_INFO("boot: %s ready (%.2f ms)", step.name, step_ms);
        } else {
            all_steps_succeeded = false;
            WOKE_LOG_ERROR("boot: %s failed (%.2f ms) - continuing in degraded mode", step.name,
                step_ms);
        }
    }

    WOKE_LOG_INFO("%s %s | %s | %s", version::kClientName, version::kVersion,
        version::kTargetGame, version::kTargetArch);
    WOKE_LOG_INFO("build %s %s", version::kBuildDate, version::kBuildTime);

    const wchar_t* session_path = woke::logger::session_log_path();
    if (session_path != nullptr && *session_path != L'\0') {
        WOKE_LOG_INFO("session log: %ls", session_path);
    }

    LARGE_INTEGER boot_end{};
    (void)::QueryPerformanceCounter(&boot_end);
    WOKE_LOG_INFO("boot complete in %.2f ms (module %p)", elapsed_ms(boot_start, boot_end, frequency),
        static_cast<void*>(self));

    if (!all_steps_succeeded) {
        WOKE_LOG_WARN("one or more boot steps failed - see ERROR lines above");
    }

    g_boot_ok = all_steps_succeeded;
}

void run_worker_loop() noexcept {
    while (!g_unload_requested.load(std::memory_order_acquire)) {
        (void)woke::logger::flush();
        ::Sleep(kWorkerIdleSleepMs);
    }
    (void)woke::logger::flush();
}

void shutdown() noexcept {
    const std::size_t dropped = woke::logger::dropped_line_count();
    if (dropped > 0) {
        WOKE_LOG_WARN("logger: %zu line(s) were dropped because the queue was saturated", dropped);
    }

    WOKE_LOG_INFO("shutdown: unloading %s %s (boot %s)", version::kClientName, version::kVersion,
        g_boot_ok ? "clean" : "degraded");

    // Reverse of the boot order. Steps are added here as the roadmap lands them.
    woke::logger::shutdown();
}

void request_unload() noexcept {
    g_unload_requested.store(true, std::memory_order_release);
}

bool unload_requested() noexcept {
    return g_unload_requested.load(std::memory_order_acquire);
}

} // namespace woke::lifecycle
