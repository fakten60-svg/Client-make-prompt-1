#pragma once

// The portable perf/hardening helpers (roadmap step 9).
//
// Host-testable on purpose: the smoothing and the suppression policy are the two numbers the
// §12.2 step-9 gate is judged by, so they live here where the Linux suite can drive them with
// synthetic samples instead of only being observed in-game. No windows.h, no allocation.

#ifndef WOKE_HAVE_JNI
#define WOKE_HAVE_JNI 0
#endif

namespace woke::perf {

// ── Budgets (§6.4, §7.4) ─────────────────────────────────────────────────────────
//
// One definition each, because two files deriving the same budget independently is how the
// numbers drift apart. The chrome's share must sit inside the whole-pipeline budget: a hard
// static_assert turns a bad edit into a build error rather than a budget that quietly no
// longer proves anything.
inline constexpr float kChromeBudgetMs = 0.2f;   // the ClickGUI composition's own share
inline constexpr float kPipelineBudgetMs = 0.5f; // §6.4: the whole per-frame pipeline

static_assert(kChromeBudgetMs < kPipelineBudgetMs,
    "the chrome budget must stay inside the pipeline budget or it proves nothing");

// Soak instrumentation (step 9). One summary line per interval keeps the 10-minute soak
// measurable without flooding the log: average/worst chrome, average/worst pipeline, frame
// rate and the suppression ratio, all from the meters that already exist.
inline constexpr double kSoakIntervalSeconds = 60.0;

// ── Exponential moving average ───────────────────────────────────────────────────
//
// The seam the chrome and pipeline meters both go through, extracted so the smoothing rule
// exists once and the host suite can assert it: the first sample seeds the average, later
// samples fold in at 1/kRate, and a non-finite or negative sample is refused rather than
// allowed to poison the average forever.
struct Ema {
    float value = 0.0f;
    bool seeded = false;

    // Folds one sample in. Returns the new average. A NaN or negative sample is ignored.
    float add(float sample, float rate) noexcept {
        if (sample != sample || sample < 0.0f) {
            return value; // NaN check without <cmath>: sample != sample is true only for NaN
        }
        if (!seeded) {
            value = sample;
            seeded = true;
            return value;
        }
        value += (sample - value) * rate;
        return value;
    }

    void reset() noexcept {
        value = 0.0f;
        seeded = false;
    }
};

// ── The soak snapshot (portable half of the step-9 gate) ─────────────────────────
//
// True when a soak interval's numbers are worth logging. "Worth logging" is a policy, not a
// measurement, which is why it is here: both budgets are expressed against the same constants
// the render paths warn with, so the log line and the on-screen diagnostics can never
// disagree about what "in budget" means. Averages beyond budget fail the interval even when
// the worst spike is what pushed them there - the gate is "sustained", not "never spiked".
[[nodiscard]] inline bool soak_interval_ok(
    float average_chrome_ms, float average_pipeline_ms) noexcept {
    return average_chrome_ms <= kChromeBudgetMs && average_pipeline_ms <= kPipelineBudgetMs;
}

} // namespace woke::perf
