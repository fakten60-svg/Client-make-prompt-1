#include "ui/gui_internal.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_win32.h>

#include "core/build_config.h"
#include "core/config.h"
#include "core/event_bus.h"
#include "core/events.h"
#include "core/logger.h"
#include "core/perf.h"
#include "core/version.h"
#include "hooks/game_thread.h"
#include "modules/module_manager.h"
#include "ui/notifications.h"
#include "ui/step8_frame.h"

// The overlay's per-frame path, its events and its lifecycle (blueprint §7.1, §7.8), plus the
// public ui/ API from gui.h. The static composition lives in gui_chrome.cpp and the in-world
// overlay in gui_hud.cpp; gui_internal.h is the contract between the three.

// ImGui's Win32 backend deliberately leaves its message handler commented out in its header (it
// does not want to pull <windows.h> into the helper), and expects the application to forward
// declare it - this is the declaration the upstream Win32 example uses. It must stay at global
// scope: the definition in imgui_impl_win32.cpp is not namespaced.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace woke::ui {
namespace detail {
namespace {

// ── Lazy renderer initialisation ──────────────────────────────────────────────────

// (Declared in gui_internal.h, defined here: the backends are this file's business because it
// owns the ImGui context lifecycle that initialises them.)

// ── The compose pass ──────────────────────────────────────────────────────────────
//
// The ClickGUI chrome for one frame: input top-down, then the window. Split out of the old
// render_overlay so the budget meter in render_overlay measures exactly the composition and
// nothing else.

void compose_frame() noexcept {
    State& s = state();
    const float open = util::clamp01(s.animation.value(s.open_spring));
    const float collapse = util::clamp01(s.animation.value(s.collapse));

    // While the chrome is fully closed, only the toast stack may still need this frame (§7.8):
    // composing the window at alpha 0 would still emit every piece of geometry it owns.
    if (!s.visible && open <= 0.001f) {
        render_notifications_layer();
        return;
    }

    const Layout layout = compute_layout(open, collapse);
    const float alpha = 1.0f - collapse;
    const ImGuiIO& io = ImGui::GetIO();
    const Rect screen{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y};

    s.header = layout.header;
    s.density_button = Rect{layout.header.right() - kDensityButtonWidth - 16.0f,
        layout.header.center_y() - (kDensityButtonHeight * 0.5f), kDensityButtonWidth,
        kDensityButtonHeight};

    // ── Input phase, top-down (§7.3). Toasts are drawn last, so they are offered the click
    // first; then the chrome, the sidebar, the search field and finally the content pane.
    s.notifications.set_screen(screen);
    (void)s.notifications.handle_input(s.input);
    handle_lights(layout);
    handle_density_button();

    if (collapse < 0.999f) {
        sync_sidebar();
        s.sidebar.set_area(layout.rail);
        s.sidebar.set_alpha(alpha);
        (void)s.sidebar.handle_input(s.input);
        if (s.sidebar.take_selection_changed()) {
            s.selected = s.sidebar.selected();
            // Leaving a page closes its drawer and disarms its capture: both belong to a card that
            // is no longer on screen.
            s.expanded_module = nullptr;
            s.capture_index = -1;
            s.slider_drag = nullptr;
            s.list_dirty = true;
        }

        s.search.set_area(search_field_rect(layout.content));
        s.search.set_alpha(alpha);
        (void)s.search.handle_input(s.input);
        if (s.search.take_changed()) {
            s.list_dirty = true;
        }
        // The registry only changes at boot in this step, but the check is what keeps a future
        // dynamic registration from showing a stale list.
        if (modules::manager().count() != s.cached_module_count) {
            s.cached_module_count = modules::manager().count();
            s.list_dirty = true;
        }
        if (s.list_dirty) {
            recompute_visible_modules();
        }

        if (is_module_category(s.selected)) {
            handle_module_cards(card_pane(layout.content), alpha);
            handle_settings_rows();
        }
    }
    handle_drag(layout);

    // Component animate() only steers targets; the controller was ticked for this frame already,
    // and the GUI owns the single tick per frame (§7.4).
    s.sidebar.animate(0.0f);
    s.search.animate(0.0f);
    for (ModuleCard& card : s.cards) {
        card.animate(0.0f);
    }

    // The drop shadow goes on the background list: it must be under the window's own fill, and it
    // is one draw group away from the window contents.
    draw::shadow_rect(ImGui::GetBackgroundDrawList(), layout.window,
        theme::metrics::kShadowSpread, util::scale_alpha(theme::color::kShadow, open));

    ImGui::SetNextWindowPos(ImVec2(layout.window.left(), layout.window.top()));
    ImGui::SetNextWindowSize(ImVec2(layout.window.w, layout.window.h));
    ImGui::SetNextWindowBgAlpha(theme::color::kWindowBackdrop.a * open);

    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse
        | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav;

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, open);
    const bool drawable = ImGui::Begin("woke.wtf##clickgui", nullptr, kFlags);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Everything is drawn manually from theme tokens, so the window itself needs no items; the
    // dummy keeps ImGui from treating a window with zero items as skippable.
    ImGui::Dummy(ImVec2(1.0f, 1.0f));

    if (drawable) {
        draw::border_stroke(draw_list, layout.window, theme::color::kWindowBorder,
            theme::metrics::kWindowRounding, theme::metrics::kWindowBorderSize);
        s.sidebar.render(draw_list, layout.rail);
        draw_content(draw_list, layout, collapse);
        draw_chrome_bar(draw_list, layout, collapse);
    }
    ImGui::End();
    ImGui::PopStyleVar();

    // Last in the frame, and outside the window: toasts are global chrome.
    render_notifications_layer();
}

// ── Events ────────────────────────────────────────────────────────────────────────

// Deliberately not noexcept: it is passed as a non-type template argument to the bus, and a
// pointer-to-noexcept-function argument for a plain function-pointer parameter is a conformance
// area worth not depending on.
void on_key_event(events::KeyEvent& event) {
    State& s = state();
    if (!event.down) {
        return;
    }

    // An armed bind capture has priority over everything else: the next key is the user's new
    // bind, not a game input and not a module toggle. A repeat cannot re-arm anything, because the
    // first key-down already consumed the capture.
    if (s.capture_index >= 0) {
        event.consumed = true;
        if (event.repeat) {
            return;
        }
        const std::size_t index = static_cast<std::size_t>(s.capture_index);
        s.capture_index = -1;
        modules::BaseModule* module =
            index < modules::manager().count() ? modules::manager().at(index) : nullptr;
        if (module == nullptr) {
            return;
        }

        auto& badge = s.cards[index].badge();
        const int previous = badge.bind_key();
        if (!badge.accept_key(event.virtual_key)) {
            return;
        }
        // ESC leaves the bind untouched; every other key rebinds. Distinguishing the two is what
        // lets the toast tell the truth instead of always claiming a rebind.
        if (badge.bind_key() == previous) {
            push_toast("Keybind", "Capture cancelled", ToastIcon::Info);
            return;
        }
        module->set_bind(badge.bind_key());
        char label[16];
        components::format_key(module->bind(), label, sizeof(label));
        FixedString<96> message;
        message.format("%s  ->  %s", module->name(), label);
        push_toast("Keybind", message.c_str(), ToastIcon::Info);
        WOKE_LOG_INFO("keybind: '%s' bound to 0x%02X (%s)", module->name(),
            static_cast<unsigned int>(module->bind()), label);
        return;
    }

    if (event.repeat || event.virtual_key != s.toggle_key) {
        return;
    }
    toggle();
    // The bind belongs to the GUI: consuming it keeps the game from also seeing RSHIFT.
    event.consumed = true;
}

void on_focus_event(events::FocusEvent& event) {
    if (event.focused || !state().visible) {
        return;
    }
    // macOS behaviour (§4.7): losing the window closes the ClickGUI. The red button and this share
    // one path, so "hidden" means exactly one thing.
    WOKE_LOG_DEBUG("gui: window lost focus - chrome hidden");
    set_visible(false);
}

} // namespace
} // namespace detail

// The public API below implements gui.h against the composition's shared state, so the detail
// names come in here once rather than qualifying every use. ensure_renderer is deliberately
// *not* pulled in by name: it is defined in this file, and a using-directive would make the
// definition and the declaration collide.
using namespace detail;
using detail::state;
using detail::State;

// ── Lazy renderer initialisation (owns the ImGui context) ─────────────────────────

bool ensure_renderer() noexcept {
    State& s = state();
    if (s.renderer_ready) {
        return true;
    }
    if (s.renderer_failed) {
        return false;
    }
    if (s.context == nullptr) {
        return false;
    }
    if (s.window == nullptr) {
        // Normal early state: the DLL can be injected before the WndProc hook has found the game
        // window. Nothing is logged per frame - the boot log already says input is unavailable.
        return false;
    }

    ImGui::SetCurrentContext(s.context);

    // The Win32 backend in its OpenGL flavour (monitor DPI + cursor handling for a GL window), and
    // then the GL3 backend. This runs inside the swap detour, where the game's GL context is
    // current by definition - the one place a GL initialisation is guaranteed to be valid.
    if (!ImGui_ImplWin32_InitForOpenGL(s.window)) {
        s.renderer_failed = true;
        WOKE_LOG_ERROR("gui-renderer: the ImGui Win32 backend could not attach to window %p",
            static_cast<void*>(s.window));
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(nullptr)) {
        ImGui_ImplWin32_Shutdown();
        s.renderer_failed = true;
        WOKE_LOG_ERROR("gui-renderer: the ImGui OpenGL3 backend could not initialise");
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // The game owns the OS cursor while it has raw input, so the overlay draws its own. Without
    // this the ClickGUI would be unusable in a captured window.
    io.MouseDrawCursor = true;

    s.renderer_ready = true;
    WOKE_LOG_INFO("gui-renderer: ImGui %s ready on window %p (win32 + opengl3)", IMGUI_VERSION,
        static_cast<void*>(s.window));
    return true;
}

// ── Public API ────────────────────────────────────────────────────────────────────

bool initialize() noexcept {
    State& s = state();
    if (s.initialized) {
        return true;
    }

    (void)::QueryPerformanceFrequency(&s.frequency);
    s.toggle_key = kDefaultToggleKey;
    s.selected = Section::Diagnostics;
    s.grid_layout = true;

    s.animation.reset();
    s.open_spring = s.animation.acquire_spring(0.0f);
    s.collapse = s.animation.acquire_state(0.0f, 12.0f);
    s.density = s.animation.acquire_state(0.0f, 16.0f);

    // Step 6: the widget library acquires every animation slot it will ever use here, once
    // (§7.4). A component handed a handle at boot cannot animate from a recycled one, and no
    // frame ever allocates a widget or a slot (§6.3). A ModuleCard owns five states (its hover,
    // its drawer reveal, the pill's t, and the badge's hover and capture pulse), which is what
    // sizes AnimationController::kMaxStates.
    s.lights.bind(s.animation);
    s.sidebar.bind(s.animation);
    s.search.bind(s.animation);
    s.notifications.bind(s.animation);
    for (ModuleCard& card : s.cards) {
        card.bind(s.animation);
    }

    s.context = ImGui::CreateContext();
    if (s.context == nullptr) {
        WOKE_LOG_ERROR("gui: ImGui context creation failed - the ClickGUI stays disabled");
        return false;
    }
    ImGui::SetCurrentContext(s.context);
    apply_imgui_style();

    s.key_subscription = events::bus().subscribe<events::KeyEvent, &on_key_event>();
    s.focus_subscription = events::bus().subscribe<events::FocusEvent, &on_focus_event>();

    s.initialized = true;
    WOKE_LOG_INFO("ui-theme: %zu sections, ImGui %s, toggle key 0x%02X", kSectionCount,
        IMGUI_VERSION, static_cast<unsigned int>(s.toggle_key));
    return true;
}

void shutdown() noexcept {
    State& s = state();
    if (!s.initialized) {
        return;
    }

    if (s.renderer_ready) {
        ImGui::SetCurrentContext(s.context);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplWin32_Shutdown();
        s.renderer_ready = false;
    }

    events::bus().unsubscribe(s.key_subscription);
    events::bus().unsubscribe(s.focus_subscription);
    s.key_subscription = events::Subscription{};
    s.focus_subscription = events::Subscription{};
    // The step-8 counter observer holds a bus slot of its own; released here so a hot unload can
    // never leave the bus calling into unmapped DLL code.
    shutdown_step8();

    // Reverse of initialize()'s bind order: every component returns its pooled slots, so a
    // re-injection (hot unload, then attach again) starts from a clean controller.
    for (ModuleCard& card : s.cards) {
        card.unbind();
    }
    s.notifications.unbind();
    s.search.unbind();
    s.sidebar.unbind();
    s.lights.unbind();

    if (s.context != nullptr) {
        ImGui::SetCurrentContext(s.context);
        ImGui::DestroyContext(s.context);
        s.context = nullptr;
    }

    hooks::game_thread::set_overlay_requested(false);
    WOKE_LOG_INFO("gui: shutdown (%zu frame(s) rendered, %zu suppressed)", s.frames_rendered,
        s.suppressed_frames);
    s.initialized = false;
}

void attach_window(void* hwnd) noexcept {
    State& s = state();
    s.window = static_cast<HWND>(hwnd);
    if (s.window != nullptr) {
        s.window_positioned = false;
    }
}

bool handle_window_message(
    void* hwnd, unsigned int message, unsigned long long wparam, long long lparam) noexcept {
    State& s = state();
    if (!s.initialized || s.context == nullptr || !s.visible) {
        return false;
    }

    ImGui::SetCurrentContext(s.context);
    (void)ImGui_ImplWin32_WndProcHandler(static_cast<HWND>(hwnd), message,
        static_cast<WPARAM>(wparam), static_cast<LPARAM>(lparam));

    switch (message) {
    case WM_MOUSEMOVE:
        // Swallowed so the camera cannot turn behind the overlay while it is open. The backend
        // already received the position above, so ImGui's own cursor stays exact.
        return true;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
#if _WIN32_WINNT >= 0x0600
    case WM_MOUSEHWHEEL:
#endif
        // Clicks and wheel are the GUI's while it is visible; the game must not mine, attack or
        // change hotbar slots behind it.
        return true;

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        // The toggle bind has to reach the event bus, which is what closes the GUI again.
        return static_cast<int>(wparam) != s.toggle_key;

    case WM_CHAR:
    case WM_SYSCHAR:
        return true; // typed characters must not reach the game's chat or inventory

    default:
        return false;
    }
}

void render_overlay() noexcept {
    State& s = state();
    if (!s.initialized) {
        return;
    }

    // §7.8. While hidden and settled this is the entire cost of a frame: one branch. No NewFrame,
    // no vertex generation, no draw-list traversal.
    if (!needs_render()) {
        ++s.suppressed_frames;
        if (!s.suppression_logged) {
            s.suppression_logged = true;
            WOKE_LOG_INFO(
                "gui-renderer: chrome hidden after %zu frame(s) - frames now return before ImGui "
                "(%zu suppressed total, zero draw calls)",
                s.frames_rendered, s.suppressed_frames);
        }
        return;
    }

    if (!ensure_renderer()) {
        // No window yet or no backend: count the frame as suppressed, because no ImGui work
        // happened, and try again next frame.
        ++s.suppressed_frames;
        return;
    }

    if (ImGui::GetCurrentContext() != s.context) {
        ImGui::SetCurrentContext(s.context);
    }

    LARGE_INTEGER start{};
    (void)::QueryPerformanceCounter(&start);

    ImGui_ImplWin32_NewFrame();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    s.input = snapshot_input();
    const double now = now_seconds();
    tick_animation(now);
    refresh_readouts(now);
    compose_frame();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    LARGE_INTEGER end{};
    (void)::QueryPerformanceCounter(&end);
    const float chrome_ms = milliseconds_between(start, end);

    ++s.frames_rendered;
    s.last_chrome_ms = chrome_ms;
    s.suppression_logged = false;
    if (s.frames_rendered == 1) {
        s.average_chrome_ms = chrome_ms;
    } else {
        s.average_chrome_ms += (chrome_ms - s.average_chrome_ms) * 0.05f;
    }
    if (chrome_ms > s.worst_chrome_ms) {
        s.worst_chrome_ms = chrome_ms;
    }

    if (s.average_chrome_ms > perf::kChromeBudgetMs
        && (now - s.last_chrome_warning_seconds) > kChromeWarningCooldownSeconds) {
        s.last_chrome_warning_seconds = now;
        WOKE_LOG_WARN("gui-renderer: chrome cost %.3f ms exceeds the %.2f ms budget (worst %.3f ms)",
            static_cast<double>(s.average_chrome_ms), static_cast<double>(perf::kChromeBudgetMs),
            static_cast<double>(s.worst_chrome_ms));
    }

    if (s.frames_rendered % kHeartbeatFrames == 0) {
        WOKE_LOG_DEBUG("gui-renderer: %zu frames, chrome avg %.3f ms, open %.2f, %zu anim slot(s)",
            s.frames_rendered, static_cast<double>(s.average_chrome_ms),
            static_cast<double>(util::clamp01(s.animation.value(s.open_spring))),
            s.animation.active_state_count());
    }
}

bool visible() noexcept {
    return state().visible;
}

void set_visible(bool requested) noexcept {
    State& s = state();
    if (s.visible == requested) {
        return;
    }
    s.visible = requested;
    if (requested) {
        // Snap the pointer state so the first frame cannot see a stale click, and clear the
        // suppression counter for the next close.
        s.suppression_logged = true;
        WOKE_LOG_INFO("gui: chrome shown (%zu section(s), grid=%s)",
            kSectionCount, s.grid_layout ? "yes" : "no");
    }
    // The frame scheduler's suppression predicate reads this (§7.8): while the chrome is visible
    // the pipeline is known to have work, independent of who is subscribed.
    hooks::game_thread::set_overlay_requested(requested);
}

void toggle() noexcept {
    set_visible(!state().visible);
}

bool needs_render() noexcept {
    const State& s = state();
    if (s.visible) {
        return true;
    }
    // Still closing: the close animation has to be rendered or it never finishes.
    if (!s.animation.at_rest(s.open_spring)) {
        return true;
    }
    // A toast outlives the chrome: the ClickGUI can be hidden while one is still on screen, and
    // that frame must not be suppressed or the toast would freeze mid-slide (§7.5, §7.8).
    if (s.notifications.needs_render()) {
        return true;
    }
    // An enabled in-world overlay module (the step-7 HUD/crosshair/trajectories, the step-8
    // Combat/Mace readouts) is visible output, so the frame that draws it must not be suppressed.
    // Without this an enabled HUD would only ever be drawn during the close animation and then
    // freeze the moment the chrome settled (§7.8).
    return inworld_overlay_wanted();
}

bool save_config() noexcept {
    if (!modules::manager().count()) {
        return false; // nothing registered: saving would write an empty document
    }
    // Config IO never runs on the render path (Section 4.2). The game thread asks; the worker
    // loop services the request and coalesces rapid toggles into one write. The synchronous
    // config::save stays reserved for the worker thread (shutdown's final-state write).
    woke::config::request_save("default");
    WOKE_LOG_DEBUG("gui: module state save requested (worker writes configs/default.json)");
    return true;
}

std::size_t registered_module_count() noexcept {
    return modules::manager().count();
}

int toggle_key() noexcept {
    return state().toggle_key;
}

void set_toggle_key(int virtual_key) noexcept {
    if (virtual_key > 0) {
        state().toggle_key = virtual_key;
    }
}

void notify(const char* title, const char* message, bool warning) noexcept {
    if (!state().initialized) {
        return;
    }
    push_toast(title != nullptr ? title : version::kClientName,
        message != nullptr ? message : "", warning ? ToastIcon::Warning : ToastIcon::Info);
}

void notify_keybind_toggle(int virtual_key) noexcept {
    const State& s = state();
    if (!s.initialized || virtual_key == 0) {
        return;
    }
    // One toast per module the key is bound to, reporting the state the keybind just produced.
    // The registry is read here rather than the caller handing in a name, so the wording stays in
    // the layer that owns the toast pool.
    const modules::ModuleManager& registry = modules::manager();
    for (std::size_t index = 0; index < registry.count(); ++index) {
        const modules::BaseModule* module = registry.at(index);
        if (module != nullptr && module->bind() == virtual_key) {
            push_module_toast(module->name(), module->enabled());
        }
    }
}

Stats stats() noexcept {
    const State& s = state();
    Stats result;
    result.initialized = s.initialized;
    result.renderer_ready = s.renderer_ready;
    result.visible = s.visible;
    result.collapsed = s.collapsed;
    result.suppressed = !needs_render();
    result.frames_rendered = s.frames_rendered;
    result.suppressed_frames = s.suppressed_frames;
    result.open_amount = util::clamp01(s.animation.value(s.open_spring));
    result.last_chrome_ms = s.last_chrome_ms;
    result.average_chrome_ms = s.average_chrome_ms;
    result.worst_chrome_ms = s.worst_chrome_ms;
    result.animation_slots = s.animation.active_state_count()
        + s.animation.active_spring_count();
    result.animation_overflows = s.animation.overflow_count();
    return result;
}

} // namespace woke::ui
