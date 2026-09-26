#pragma once

// Soak instrumentation (roadmap step 9, gate §12.2: "10-min soak: overlay avg <0.5 ms;
// inject/eject x20 stable; no local-ref leaks (log line)").
//
// The gate wants evidence in the log, so this file produces it: once per soak interval it
// emits one summary line with the pipeline and chrome meters, the suppression ratio, the
// frame counts and - when the JVM bridge is built - the ScopedLocalFrame failure counters
// (risk R-06). An interval whose averages sit inside the §6.4 budgets passes; one that
// exceeds them is logged as a WARN at the same cadence, so a soak transcript can be judged
// by grepping for a single marker string.
//
// The tick is driven by the frame scheduler's TickEvent, so the soak clock is the game's own
// 20 Hz tick and costs one comparison per tick.

#ifndef _WIN32
#error "core/soak.h is Windows-only; portable translation units must not include it."
#endif

#include <cstdint>

namespace woke::perf {

// Starts the soak reporter (subscribes a tick observer). Idempotent.
void start_soak() noexcept;

// Stops the reporter and emits one final line with the session totals.
void stop_soak() noexcept;

// Forces one interval report now (the shutdown path uses this so a short session still
// gets its summary). Safe to call before start_soak(); it then does nothing.
void report_soak_now() noexcept;

// True once the reporter has emitted at least one interval line.
[[nodiscard]] bool soak_reported() noexcept;

struct SoakInterval {
    bool ok = false;                 // averages inside both budgets
    float average_chrome_ms = 0.0f;
    float worst_chrome_ms = 0.0f;
    float average_pipeline_ms = 0.0f;
    float worst_pipeline_ms = 0.0f;
    std::uint64_t frames = 0;        // rendered in the interval
    std::uint64_t suppressed = 0;    // suppressed in the interval
    std::uint64_t local_push_failures = 0;
    std::uint64_t local_pop_failures = 0;
};

// Composes the interval snapshot from the live meters. Split from the logging so the host
// suite (and the shutdown path) can inspect the numbers the log line is built from.
[[nodiscard]] SoakInterval soak_snapshot() noexcept;

} // namespace woke::perf
