#include "hooks/game_thread.h"

#include <windows.h>

#include <array>

#include "core/build_config.h"
#include "core/event_bus.h"
#include "core/events.h"
#include "core/logger.h"

#if WOKE_HAVE_JNI
#include "jni/game_instance.h"
#endif

namespace woke::hooks::game_thread {
namespace {

// The tick rate matters more than the frame rate for game-state reads: modules read and write
// client state at exactly the game's own tick cadence (§4.8).
constexpr double kTickIntervalSeconds = 1.0 / 20.0;

// A frame delta beyond this is a stall (debugger, alt-tab, asset reload), not a slow frame.
// Clamping keeps one stall from firing a burst of ticks and from advancing animations by a
// quarter of a second in a single step.
constexpr double kMaxDeltaSeconds = 0.25;

constexpr double kFramesPerSecondWindowSeconds = 0.5;

// Overlay budget from §6.4. Measured, not estimated: the meter wraps the whole pipeline.
constexpr float kPipelineBudgetMs = 0.5f;
constexpr std::size_t kPerfWindowFrames = 120;
constexpr int kSustainedFramesOverBudget = 30;
constexpr double kPerfWarningCooldownSeconds = 10.0;

// Periodic DEBUG heartbeat. DEBUG goes to the session file, not the console, so a long
// session can be diagnosed afterwards without a single line of noise during play.
constexpr std::uint64_t kHeartbeatFrames = 600;

struct State {
    bool running = false;
    bool overlay_requested = false;
    bool frame_logged = false;

    LARGE_INTEGER frequency{};
    double last_frame_seconds = 0.0;
    bool have_frame_time = false;

    double tick_accumulator = 0.0;
    std::uint64_t frame_index = 0;
    std::uint64_t tick_index = 0;

    double fps_accumulator = 0.0;
    unsigned int fps_frames = 0;
    float frames_per_second = 0.0f;

    std::array<float, kPerfWindowFrames> samples{};
    std::size_t sample_count = 0;
    std::size_t sample_cursor = 0;
    float sample_sum = 0.0f;
    float worst_ms = 0.0f;
    int over_budget_streak = 0;
    double last_warning_seconds = 0.0;
    std::size_t perf_warnings = 0;

    bool world_present = false;
    bool player_present = false;

    events::Subscription frame_observer{};
};

State g_state{};

double now_seconds() noexcept {
    if (g_state.frequency.QuadPart <= 0) {
        return 0.0;
    }
    LARGE_INTEGER counter{};
    (void)::QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(g_state.frequency.QuadPart);
}

float elapsed_ms(const LARGE_INTEGER& start, const LARGE_INTEGER& end) noexcept {
    if (g_state.frequency.QuadPart <= 0) {
        return 0.0f;
    }
    const double ticks = static_cast<double>(end.QuadPart - start.QuadPart);
    return static_cast<float>((ticks * 1000.0) / static_cast<double>(g_state.frequency.QuadPart));
}

float average_pipeline_ms() noexcept {
    return g_state.sample_count == 0
        ? 0.0f
        : (g_state.sample_sum / static_cast<float>(g_state.sample_count));
}

// The built-in observer is what makes step 3's gate observable: it proves the frame pipeline
// is live and keeps the periodic numbers in the session log. It is also the reason the
// scheduler has something to do before step 4 introduces the GUI.
// Deliberately not noexcept: it is passed as a non-type template argument to the bus, and a
// 'pointer to noexcept function' argument for a plain function-pointer parameter is a
// conformance area worth not depending on.
void on_frame_event(events::FrameEvent& frame) {
    if (!g_state.frame_logged) {
        g_state.frame_logged = true;
        WOKE_LOG_INFO(
            "frame-pipeline: first frame observed (swap hook is live, frame %llu)",
            static_cast<unsigned long long>(frame.frame_index));
    }

    if (frame.frame_index % kHeartbeatFrames == 0) {
        WOKE_LOG_DEBUG("frame-pipeline: %llu frames, %.1f fps, avg %.3f ms, worst %.3f ms",
            static_cast<unsigned long long>(frame.frame_index),
            static_cast<double>(frame.frames_per_second),
            static_cast<double>(average_pipeline_ms()), static_cast<double>(g_state.worst_ms));
    }
}

void update_frames_per_second(double delta) noexcept {
    g_state.fps_accumulator += delta;
    ++g_state.fps_frames;
    if (g_state.fps_accumulator < kFramesPerSecondWindowSeconds) {
        return;
    }
    g_state.frames_per_second = static_cast<float>(
        static_cast<double>(g_state.fps_frames) / g_state.fps_accumulator);
    g_state.fps_accumulator = 0.0;
    g_state.fps_frames = 0;
}

void record_pipeline_sample(float milliseconds, double now) noexcept {
    if (g_state.sample_count < kPerfWindowFrames) {
        g_state.samples[g_state.sample_count] = milliseconds;
        ++g_state.sample_count;
        g_state.sample_sum += milliseconds;
        g_state.sample_cursor = g_state.sample_count % kPerfWindowFrames;
    } else {
        g_state.sample_sum -= g_state.samples[g_state.sample_cursor];
        g_state.samples[g_state.sample_cursor] = milliseconds;
        g_state.sample_sum += milliseconds;
        g_state.sample_cursor = (g_state.sample_cursor + 1) % kPerfWindowFrames;
    }

    if (milliseconds > g_state.worst_ms) {
        g_state.worst_ms = milliseconds;
    }

    g_state.over_budget_streak = (milliseconds > kPipelineBudgetMs) ? g_state.over_budget_streak + 1 : 0;
    if (g_state.over_budget_streak < kSustainedFramesOverBudget
        || (now - g_state.last_warning_seconds) < kPerfWarningCooldownSeconds) {
        return;
    }

    // Throttled: a sustained overrun warns once per cooldown instead of every frame, because a
    // log flood inside the render path is worse than the overrun it reports.
    g_state.last_warning_seconds = now;
    g_state.over_budget_streak = 0;
    ++g_state.perf_warnings;
    WOKE_LOG_WARN(
        "frame-pipeline: budget exceeded (avg %.3f ms, worst %.3f ms over %zu frames, budget %.1f ms)",
        static_cast<double>(average_pipeline_ms()), static_cast<double>(g_state.worst_ms),
        g_state.sample_count, static_cast<double>(kPipelineBudgetMs));
}

void watch_world_state() noexcept {
#if WOKE_HAVE_JNI
    // Read on the game thread only, which is exactly why the swap hook is the JNI access point
    // (D-04): no other thread can observe the world mid-tick.
    const bool world_here = game::in_world();
    const bool player_here = game::has_player();

    if (world_here != g_state.world_present) {
        g_state.world_present = world_here;
        events::WorldChangeEvent event;
        event.joined = world_here;
        events::bus().post(event);
        WOKE_LOG_INFO("frame-pipeline: world %s", world_here ? "joined" : "left");
    }

    if (player_here != g_state.player_present) {
        g_state.player_present = player_here;
        events::PlayerChangeEvent event;
        event.present = player_here;
        events::bus().post(event);
        WOKE_LOG_DEBUG("frame-pipeline: player %s", player_here ? "present" : "absent");
    }
#endif
}

} // namespace

void start() noexcept {
    if (g_state.running) {
        return;
    }

    g_state = State{};
    (void)::QueryPerformanceFrequency(&g_state.frequency);
    if (g_state.frequency.QuadPart <= 0) {
        WOKE_LOG_WARN("frame-pipeline: no high-resolution clock available - timing is disabled");
        g_state.frequency.QuadPart = 0;
    }

    g_state.running = true;
    g_state.frame_observer = events::bus().subscribe<events::FrameEvent, &on_frame_event>();
    if (!g_state.frame_observer.valid()) {
        WOKE_LOG_WARN("frame-pipeline: the frame observer could not subscribe");
    }

    WOKE_LOG_INFO("frame-pipeline: running (tick %.0f Hz, overlay budget %.1f ms, %zu subscriber(s))",
        kTickIntervalSeconds > 0.0 ? 1.0 / kTickIntervalSeconds : 0.0,
        static_cast<double>(kPipelineBudgetMs), events::bus().handler_count<events::FrameEvent>());
}

void stop() noexcept {
    if (!g_state.running) {
        return;
    }

    events::bus().unsubscribe(g_state.frame_observer);
    g_state.frame_observer = events::Subscription{};
    g_state.running = false;

    WOKE_LOG_INFO("frame-pipeline: stopped after %llu frame(s) and %llu tick(s), %zu perf warning(s)",
        static_cast<unsigned long long>(g_state.frame_index),
        static_cast<unsigned long long>(g_state.tick_index), g_state.perf_warnings);
}

void on_frame() noexcept {
    if (!g_state.running) {
        return;
    }

    const double now = now_seconds();
    double delta = g_state.have_frame_time ? (now - g_state.last_frame_seconds) : 0.0;
    g_state.last_frame_seconds = now;
    g_state.have_frame_time = true;
    if (delta < 0.0) {
        delta = 0.0;
    }
    if (delta > kMaxDeltaSeconds) {
        delta = kMaxDeltaSeconds;
    }

    // Suppression contract: nothing subscribed and no overlay means nothing to do. The tick
    // accumulator is dropped with it so a long suppressed period cannot fire a burst of ticks
    // the moment the GUI opens.
    if (!needs_frame_work()) {
        g_state.tick_accumulator = 0.0;
        return;
    }

    LARGE_INTEGER pipeline_start{};
    (void)::QueryPerformanceCounter(&pipeline_start);

    events::Bus& bus = events::bus();

    ++g_state.frame_index;
    events::FrameEvent frame;
    frame.delta_seconds = static_cast<float>(delta);
    frame.frame_index = g_state.frame_index;
    frame.frames_per_second = g_state.frames_per_second;
    bus.post(frame);

    bool ticked = false;
    g_state.tick_accumulator += delta;
    while (g_state.tick_accumulator >= kTickIntervalSeconds) {
        g_state.tick_accumulator -= kTickIntervalSeconds;
        ++g_state.tick_index;
        ticked = true;

        events::TickEvent tick;
        tick.delta_seconds = static_cast<float>(kTickIntervalSeconds);
        tick.tick_index = g_state.tick_index;
        bus.post(tick);
    }

    update_frames_per_second(delta);

    if (ticked) {
        // The game-state watch runs at the tick cadence, not per frame: two JNI field reads
        // per frame for a value that can only change between ticks would be pure waste.
        watch_world_state();
    }

    LARGE_INTEGER pipeline_end{};
    (void)::QueryPerformanceCounter(&pipeline_end);
    record_pipeline_sample(elapsed_ms(pipeline_start, pipeline_end), now);
}

bool running() noexcept {
    return g_state.running;
}

bool needs_frame_work() noexcept {
    if (g_state.overlay_requested) {
        return true;
    }
    const events::Bus& bus = events::bus();
    return bus.handler_count<events::FrameEvent>() > 0 || bus.handler_count<events::TickEvent>() > 0;
}

bool overlay_requested() noexcept {
    return g_state.overlay_requested;
}

void set_overlay_requested(bool requested) noexcept {
    g_state.overlay_requested = requested;
}

Stats stats() noexcept {
    Stats result;
    result.frames = g_state.frame_index;
    result.ticks = g_state.tick_index;
    result.frames_per_second = g_state.frames_per_second;
    result.average_pipeline_ms = average_pipeline_ms();
    result.worst_pipeline_ms = g_state.worst_ms;
    result.perf_warnings = g_state.perf_warnings;
    return result;
}

} // namespace woke::hooks::game_thread
