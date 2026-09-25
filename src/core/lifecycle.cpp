#include "core/lifecycle.h"

#include <atomic>

#include "core/build_config.h"
#include "core/config.h"
#include "core/event_bus.h"
#include "core/events.h"
#include "core/logger.h"
#include "core/version.h"
#include "modules/examples.h"
#include "modules/game_writes.h"
#include "modules/misc/clickgui.h"
#include "modules/misc/config_hotkeys.h"
#include "modules/misc/panic.h"
#include "modules/module_manager.h"
#include "modules/movement/auto_sprint.h"
#include "modules/movement/velocity_display.h"
#include "modules/visual/custom_crosshair.h"
#include "modules/visual/fullbright.h"
#include "modules/visual/hud_module.h"
#include "modules/visual/trajectories.h"
#include "modules/visual/zoom.h"
#include "hooks/game_thread.h"
#include "hooks/hook_manager.h"
#include "hooks/swap_hook.h"
#include "hooks/wndproc_hook.h"
#include "jni/mappings.h"
#include "ui/gui.h"
#include "utils/win32_utils.h"

#if WOKE_HAVE_JNI
#include "jni/game_instance.h"
#include "jni/game_writes_jni.h"
#include "jni/jni_context.h"
#include "jni/reflection_cache.h"
#endif

namespace woke::lifecycle {
namespace {

constexpr DWORD kWorkerIdleSleepMs = 100;   // ~10 Hz flush cadence
constexpr int kGameInstanceRetryBudget = 300; // 30 s at the idle cadence

std::atomic<bool> g_unload_requested{false};
bool g_boot_ok = false;

// The module objects. Fixed at boot, owned by this translation unit, registered into the
// process-wide manager; the config engine and the GUI reach them only through the registry.
//
// Registration order is display order within a category, so these are listed in the §8 catalogue's
// order. Step 8 adds the Movement set (Auto Sprint, Velocity Display) and the two Misc action
// entries (Panic, Config Hotkeys); the two step-5 example modules stay registered because the
// config and GUI test fixtures are built on them.
woke::modules::SprintState g_sprint_module;
woke::modules::movement::AutoSprint g_auto_sprint_module;
woke::modules::movement::VelocityDisplay g_velocity_display_module;
woke::modules::misc::ClickGUI g_clickgui_module;
woke::modules::misc::Panic g_panic_module;
woke::modules::misc::ConfigHotkeys g_config_hotkeys_module;
woke::modules::visual::Fullbright g_fullbright_module;
woke::modules::visual::HudModule g_hud_module;
woke::modules::visual::Zoom g_zoom_module;
woke::modules::visual::Trajectories g_trajectories_module;
woke::modules::visual::CustomCrosshair g_crosshair_module;

// Module fan-out + keybind subscriptions, owned by the modules boot step (released at shutdown).
woke::events::Subscription g_keybind_subscription{};
woke::events::Subscription g_tick_subscription{};

// The mapping registry is the one core object that outlives its own boot step: the JVM
// bridge resolves every handle through it for the whole session.
woke::jni::Mappings g_mappings;

using StepFn = bool (*)() noexcept;

struct Step {
    // 'requires' is a C++20 keyword, so the dependency field is named for what it holds.
    const char* name;
    StepFn run;
    int requires_step; // index of a step that must have succeeded first, or -1
};

// ── Boot steps ───────────────────────────────────────────────────────────────────
// Order is fixed and owned here. Later roadmap steps append their stage to this table
// (hooks -> theme -> modules -> config) instead of self-initialising anywhere else, which is
// what keeps boot and hot-unload symmetric.

bool start_logger() noexcept {
    return woke::logger::init();
}

bool start_mappings() noexcept {
    const std::wstring path = util::resolve_runtime_path(L"mappings.json");
    if (path.empty()) {
        WOKE_LOG_ERROR("mappings: no runtime path could be resolved for mappings.json");
        return false;
    }

    const std::string text = util::read_text_file(path);
    if (text.empty()) {
        WOKE_LOG_ERROR("mappings: '%ls' is missing or unreadable", path.c_str());
        return false;
    }

    if (!g_mappings.parse(text)) {
        WOKE_LOG_ERROR("mappings: '%ls' could not be parsed", path.c_str());
    }

    for (const std::string& warning : g_mappings.warnings()) {
        WOKE_LOG_WARN("%s", warning.c_str());
    }
    if (g_mappings.warnings_truncated()) {
        WOKE_LOG_WARN("mappings: warning list was truncated - check the asset for more");
    }
    if (!g_mappings.loaded()) {
        WOKE_LOG_ERROR("mappings: no class could be registered - JNI modules stay disabled");
        return false;
    }

    WOKE_LOG_INFO("mappings: resolved %zu classes / %zu methods / %zu fields (%s)",
        g_mappings.class_count(), g_mappings.method_count(), g_mappings.field_count(),
        g_mappings.source().empty() ? "unknown source" : g_mappings.source().c_str());

    // Anchors are the identifiers the core is built around: verify them here so a stale
    // asset is reported at boot rather than as a mystery failure inside a module.
    const woke::jni::AnchorStatus anchors = g_mappings.verify_anchors();
    if (anchors.all_ok()) {
        WOKE_LOG_INFO("mappings: %zu/%zu anchor identifiers verified", anchors.resolved,
            anchors.checks.size());
    } else {
        WOKE_LOG_WARN("mappings: %zu of %zu anchors unresolved, %zu mismatched", anchors.failed,
            anchors.checks.size(), anchors.mismatched);
        for (const woke::jni::AnchorCheck& check : anchors.checks) {
            if (check.resolved && check.matches_expected) {
                continue;
            }
            WOKE_LOG_WARN("mappings: anchor %s.%s -> expected %s, %s", check.class_alias.c_str(),
                check.member.empty() ? "(class)" : check.member.c_str(),
                check.expected_intermediary.empty() ? "(any)" : check.expected_intermediary.c_str(),
                check.resolved ? "different identifier" : "not found");
        }
    }
    return true;
}

#if WOKE_HAVE_JNI

bool start_jvm_bridge() noexcept {
    if (!jni::initialize()) {
        WOKE_LOG_ERROR("jvm-bridge: no JVM in this process - game state stays unreadable");
        return false;
    }

    jni::bind_registry(g_mappings);

    // A missing client instance is not a boot failure: it is the normal state when the DLL
    // is injected before the game reaches its first world. The worker loop retries.
    if (!game::initialize()) {
        WOKE_LOG_WARN("jvm-bridge: attached, waiting for a live MinecraftClient instance");
        return false;
    }

    const jni::CacheStats cache = jni::stats();
    WOKE_LOG_INFO("jvm-bridge: %zu classes / %zu methods / %zu fields resolved, %zu failure(s)",
        cache.classes, cache.methods, cache.fields, cache.failures);
    return true;
}

#else

bool start_jvm_bridge() noexcept {
    WOKE_LOG_WARN("jvm-bridge: skipped - built without JDK JNI headers");
    return false;
}

#endif

// ── Step 3: the hook engine ──────────────────────────────────────────────────────

bool start_event_bus() noexcept {
    // A fresh bus at boot is what makes re-injection safe: a stale subscriber from a previous
    // load would otherwise be called with a context that no longer exists.
    woke::events::bus().reset();
    WOKE_LOG_INFO("event-bus: ready (%zu channels x %zu slots per channel)",
        woke::events::Bus::kMaxChannels, woke::events::Bus::kSlotsPerChannel);
    return true;
}

bool start_frame_scheduler() noexcept {
    hooks::game_thread::start();
    return hooks::game_thread::running();
}

bool start_hook_engine() noexcept {
    return hooks::initialize();
}

bool start_swap_hook() noexcept {
    return hooks::install_swap_hook();
}

bool start_wndproc_hook() noexcept {
    return hooks::install_wndproc_hook();
}

// ── Step 4: the ClickGUI ────────────────────────────────────────────────────────

bool start_ui() noexcept {
    // Creates the ImGui context, applies the theme tokens and subscribes the GUI's own input
    // handlers. No window and no GL context are touched: the backends are initialised lazily
    // inside the swap trampoline, which is the only place a GL initialisation is valid.
    return ui::initialize();
}

bool start_ui_window() noexcept {
    // Hands the subclassed window to the overlay. A missing window is the documented degraded
    // mode (R-02): the GUI stays dark and the client keeps running.
    ui::attach_window(hooks::window_handle());
    if (hooks::window_handle() == nullptr) {
        WOKE_LOG_WARN("gui: no game window yet - the overlay waits for the renderer backend");
        return false;
    }
    return true;
}

// ── Step 5: the module system ──────────────────────────────────────────────

// Keybind dispatch (§4.4): the WndProc's KeyEvent stream feeds the module manager after the GUI
// has had its chance to consume the key. The GUI subscribes first (it is a boot step ahead), so
// `consumed` here means "the ClickGUI already took this key" - either because it is the toggle
// bind or because a keybind chip is capturing - and the module binds must not also act on it.
//
// Both edges are forwarded since step 8: a press-only dispatcher cannot express a hold bind
// (ClickGUI's hold mode, a future paddle), and a *released* key is exactly the event a hold bind
// needs most. Only key-*down* is GUI-suppressed (§4.4), so a bind held while the overlay opens
// still receives its release instead of sticking on.
void dispatch_module_keybind(events::KeyEvent& event) {
    if (event.consumed) {
        return;
    }
    if (event.down) {
        if (event.repeat) {
            return;
        }
        // GUI-open suppression (§4.4). Typing in the search field or clicking through the cards
        // must not toggle modules behind the overlay. The ClickGUI's own toggle bind is
        // structurally exempt: it is handled by the GUI's handler above, never by a module bind.
        if (ui::visible()) {
            return;
        }
    }
    if (modules::manager().handle_key(event.virtual_key, event.down)) {
        event.consumed = true;
        // §4.4: a module state change is always user-visible. The dispatcher reports the key that
        // was pressed and the overlay - which owns the toast pool - resolves the module names, so
        // the wording stays in one layer. Action modules are filtered out by has_toggle_bind():
        // their key does something rather than flipping a state, so there is nothing to announce.
        if (event.down && modules::manager().has_toggle_bind(event.virtual_key)) {
            ui::notify_keybind_toggle(event.virtual_key);
        }
    }
}

// The registry's tick fan-out: enabled modules receive the tick at the game's 20 Hz cadence.
// Rendering fan-out (on_render) is a no-op for the two step-5 example modules, but the plumbing
// is live so step 7's visual modules do not need to touch the scheduler.
void dispatch_tick(events::TickEvent& event) noexcept {
    // A profile load a hotkey requested lands here, on the game thread at a frame boundary, rather
    // than inside key dispatch: applying a config flips module enable state, and mutating the
    // registry while the key fan-out is walking it is the ordering bug the load mailbox removes.
    config::LoadReport applied;
    if (config::service_loads(applied)) {
        WOKE_LOG_INFO(
            "modules: profile loaded (%zu module(s) matched, %zu value(s) applied, %zu unknown)",
            applied.modules_matched, applied.settings_applied, applied.unknown_modules);
    }
    modules::manager().on_tick(event.delta_seconds);
}

bool start_modules() noexcept {
    woke::modules::ModuleManager& registry = woke::modules::manager();
    registry.reset();

    // Point the write seam at the real game options before the config load, so a Fullbright or
    // Zoom that a saved config re-enables writes through the bridge on its very first tick. With
    // the bridge compiled out the portable recorder stays installed and those modules refuse to
    // enable, which is the honest "no game to affect" behaviour.
#if WOKE_HAVE_JNI
    (void)woke::modules::set_game_writes(&woke::jni::jni_game_writes());
#endif

    const bool registered = registry.add(&g_sprint_module) && registry.add(&g_auto_sprint_module)
        && registry.add(&g_velocity_display_module) && registry.add(&g_clickgui_module)
        && registry.add(&g_panic_module) && registry.add(&g_config_hotkeys_module)
        && registry.add(&g_fullbright_module) && registry.add(&g_hud_module)
        && registry.add(&g_zoom_module) && registry.add(&g_trajectories_module)
        && registry.add(&g_crosshair_module);
    if (!registered) {
        WOKE_LOG_ERROR("modules: registration failed");
        return false;
    }

    // Restore the last session's state (configs/default.json). First boot keeps defaults: a
    // missing file is the normal state, not an error.
    const config::LoadReport loaded = config::load("default");
    if (!loaded.parsed) {
        WOKE_LOG_INFO("modules: no saved config - starting from defaults");
    } else {
        WOKE_LOG_INFO(
            "modules: config loaded (%zu module(s) matched, %zu value(s) applied, %zu unknown "
            "module(s), %zu unknown setting(s), %zu wrong-typed)",
            loaded.modules_matched, loaded.settings_applied, loaded.unknown_modules,
            loaded.unknown_settings, loaded.wrong_type);
    }

    WOKE_LOG_INFO("modules: %zu registered, %zu enabled", registry.count(),
        registry.enabled_count());

    // The subscriptions are taken after the config load so a load cannot race the first key.
    g_keybind_subscription = woke::events::bus().subscribe<events::KeyEvent, &dispatch_module_keybind>();
    g_tick_subscription = woke::events::bus().subscribe<events::TickEvent, &dispatch_tick>();
    return true;
}

bool save_default_config() noexcept {
    if (!config::save("default")) {
        WOKE_LOG_WARN("modules: the default config could not be written");
        return false;
    }
    WOKE_LOG_INFO("modules: default config saved");
    return true;
}

constexpr Step kBootSteps[] = {
    // index   step                run                      depends on
    {"logger", &start_logger, -1},
    {"mappings", &start_mappings, 0},
    {"jvm-bridge", &start_jvm_bridge, 1},
    {"event-bus", &start_event_bus, -1},
    // The scheduler runs before the hook that will drive it, so the very first swap already
    // has a scheduler to hand the frame to.
    {"frame-scheduler", &start_frame_scheduler, 3},
    {"hook-engine", &start_hook_engine, -1},
    {"swap-hook", &start_swap_hook, 5},
    {"wndproc-hook", &start_wndproc_hook, 5},
    // The GUI needs the bus (for its keybind and focus), and it needs the window before it can
    // render, but neither order is a hard dependency: a GUI without a window simply stays dark.
    {"ui", &start_ui, 3},
    {"ui-window", &start_ui_window, 7},
    // The registry feeds the GUI's category pages, so it must exist before the first frame; the
    // config load needs only the registry itself.
    {"modules", &start_modules, 3},
};

constexpr std::size_t kStepCount = sizeof(kBootSteps) / sizeof(kBootSteps[0]);

double elapsed_ms(
    const LARGE_INTEGER& start, const LARGE_INTEGER& end, const LARGE_INTEGER& frequency) {
    if (frequency.QuadPart == 0) {
        return 0.0;
    }
    const double ticks = static_cast<double>(end.QuadPart - start.QuadPart);
    return (ticks * 1000.0) / static_cast<double>(frequency.QuadPart);
}

// Late-bound retry: the DLL may be injected before the game has created its client
// instance, so this runs at the worker cadence until it succeeds or the budget is spent.
void retry_game_instance() noexcept {
#if WOKE_HAVE_JNI
    if (game::available()) {
        return;
    }
    static int attempts = 0;
    if (attempts >= kGameInstanceRetryBudget) {
        return;
    }
    ++attempts;
    if (game::initialize()) {
        WOKE_LOG_INFO("jvm-bridge: client instance acquired after %d retry attempt(s)", attempts);
    } else if (attempts == kGameInstanceRetryBudget) {
        WOKE_LOG_WARN("jvm-bridge: gave up waiting for a client instance after %d attempts",
            attempts);
    }
#endif
}

} // namespace

void boot(HMODULE self) noexcept {
    LARGE_INTEGER frequency{};
    (void)::QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER boot_start{};
    (void)::QueryPerformanceCounter(&boot_start);

    bool step_ok[kStepCount] = {};
    for (std::size_t step_index = 0; step_index < kStepCount; ++step_index) {
        const Step& step = kBootSteps[step_index];
        if (step.requires_step >= 0 && !step_ok[step.requires_step]) {
            WOKE_LOG_WARN("boot: %s skipped (depends on %s)", step.name,
                kBootSteps[step.requires_step].name);
            continue;
        }

        LARGE_INTEGER step_start{};
        (void)::QueryPerformanceCounter(&step_start);
        step_ok[step_index] = step.run();
        LARGE_INTEGER step_end{};
        (void)::QueryPerformanceCounter(&step_end);

        const double step_ms = elapsed_ms(step_start, step_end, frequency);
        if (step_ok[step_index]) {
            WOKE_LOG_INFO("boot: %s ready (%.2f ms)", step.name, step_ms);
        } else {
            WOKE_LOG_WARN("boot: %s failed (%.2f ms) - continuing in degraded mode", step.name,
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

    WOKE_LOG_INFO("hooks: %zu active, %zu queued for removal | %s", hooks::active_hook_count(),
        hooks::queued_removal_count(), hooks::wndproc_hook_installed() ? "input routed" : "input unavailable");

    const ui::Stats overlay = ui::stats();
    WOKE_LOG_INFO("gui: %s | %zu animation slot(s) | toggle key 0x%02X",
        overlay.initialized ? (overlay.renderer_ready ? "renderer ready" : "waiting for the game window")
                            : "disabled",
        overlay.animation_slots, static_cast<unsigned int>(ui::toggle_key()));

#if WOKE_HAVE_JNI
    WOKE_LOG_INFO("jvm-bridge: %zu thread(s) attached, %zu handle(s) unresolved",
        jni::attached_thread_count(), game::unresolved_handle_count());
#endif

    g_boot_ok = true;
    for (std::size_t index = 0; index < kStepCount; ++index) {
        g_boot_ok = g_boot_ok && step_ok[index];
    }

    LARGE_INTEGER boot_end{};
    (void)::QueryPerformanceCounter(&boot_end);
    WOKE_LOG_INFO("boot complete in %.2f ms (module %p)", elapsed_ms(boot_start, boot_end, frequency),
        static_cast<void*>(self));

    if (!g_boot_ok) {
        WOKE_LOG_WARN("one or more boot steps failed - see the lines above");
    }
}

void run_worker_loop() noexcept {
    while (!g_unload_requested.load(std::memory_order_acquire)) {
        // Deferred module saves (§4.2) are serviced here, on the worker thread: a GUI toggle
        // or keybind posts a request instead of writing config files on the render path.
        if (woke::config::service_saves()) {
            WOKE_LOG_DEBUG("modules: deferred config save written");
        }
        (void)woke::logger::flush();
        retry_game_instance();
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

    // Reverse of the boot order (§4.5). The window procedure is restored before MinHook shuts
    // down, and both hooks are disabled before the engine is uninitialised: after this point
    // nothing can enter a trampoline, which is what makes a hot unload safe.
    hooks::game_thread::stop();
    hooks::remove_wndproc_hook();
    hooks::remove_swap_hook();

    // The session's final state lands in configs/default.json before anything unsubscribes:
    // config IO runs on this (worker) thread, never in on_tick/on_render (§4.2).
    (void)save_default_config();

    // Undo every module's live side effect while the bridge is still up. Fullbright and Zoom
    // write a game option, so unloading with them enabled would otherwise leave the running game
    // at the modified gamma or field of view. The config above already captured the enabled state,
    // so the next injection restores it deliberately rather than by accident.
    const std::size_t still_live = woke::modules::manager().disable_all();
    if (still_live > 0) {
        WOKE_LOG_INFO("modules: %zu live side effect(s) reverted before unload", still_live);
    }
#if WOKE_HAVE_JNI
    (void)woke::modules::set_game_writes(nullptr);
#endif

    // Module dispatch stops with the bus reset (below); the subscriptions are dropped first so
    // no tick or key can reach the registry while its modules are being torn down.
    woke::events::bus().unsubscribe(g_keybind_subscription);
    woke::events::bus().unsubscribe(g_tick_subscription);
    g_keybind_subscription = woke::events::Subscription{};
    g_tick_subscription = woke::events::Subscription{};

    // With the swap hook gone nothing can call render_overlay() and with the subclass restored
    // nothing can call handle_window_message(), so the overlay can release its GL objects and its
    // ImGui context without racing the render thread.
    ui::shutdown();
    hooks::shutdown();

    // Then the game façade releases its global ref to the client, the cache releases every
    // class reference, and the threads detach.
#if WOKE_HAVE_JNI
    game::shutdown();
    jni::unbind_registry();
    jni::shutdown();
#endif

    woke::logger::shutdown();
}

void request_unload() noexcept {
    g_unload_requested.store(true, std::memory_order_release);
}

bool unload_requested() noexcept {
    return g_unload_requested.load(std::memory_order_acquire);
}

} // namespace woke::lifecycle
