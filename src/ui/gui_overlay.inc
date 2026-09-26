// ── In-world overlay (blueprint §7.6, roadmap step 7) ────────────────────────────

using modules::visual::CustomCrosshair;
using modules::visual::HudModule;
using modules::movement::VelocityDisplay;
using modules::visual::Trajectories;

// The in-world overlay modules, resolved by name and re-resolved only when the registry size
// changes. Cached because the draw path asks about them every frame, and a name lookup per frame
// would be thirty-odd string compares for a fact that changes at most once per click.
struct OverlayModules {
    const HudModule* hud = nullptr;
    const CustomCrosshair* crosshair = nullptr;
    const Trajectories* trajectories = nullptr;
    const VelocityDisplay* velocity = nullptr;
    std::size_t resolved_count = static_cast<std::size_t>(-1);
};

OverlayModules g_overlay{};

void resolve_overlay_modules() noexcept {
    const std::size_t count = modules::manager().count();
    if (count == g_overlay.resolved_count) {
        return;
    }
    g_overlay.resolved_count = count;
    // dynamic_cast, not static_cast: the lookup is by name, so the cast is only sound while that
    // name still maps to the type it claims to. A future name collision becomes a null pointer - a
    // silent overlay that does not draw - instead of undefined behaviour.
    g_overlay.hud = dynamic_cast<const HudModule*>(modules::manager().find("HUD"));
    g_overlay.crosshair =
        dynamic_cast<const CustomCrosshair*>(modules::manager().find("Custom Crosshair"));
    g_overlay.trajectories =
        dynamic_cast<const Trajectories*>(modules::manager().find("Trajectories"));
    g_overlay.velocity =
        dynamic_cast<const VelocityDisplay*>(modules::manager().find("Velocity Display"));
}

// True when an enabled overlay module would draw. ui::needs_render() consults this so an enabled
// HUD keeps the frame pipeline alive with the chrome hidden (§7.6, §7.8). The policy itself lives
// in overlay.cpp - portable, host-tested, and the same predicate the frame scheduler's flag reads -
// so this composition only asks instead of keeping a second copy of the rules.
bool inworld_overlay_wanted() noexcept {
    return overlay::wanted();
}

// The live player view for the trajectory prediction. Read only when a path is actually going to
// be drawn: four JNI calls per frame for a hidden overlay would be pure waste.
void read_player_view(hud::Frame& frame) noexcept {
#if WOKE_HAVE_JNI
    const auto eye = game::player_eye_position();
    const auto yaw = game::player_yaw();
    const auto pitch = game::player_pitch();
    const auto velocity = game::player_velocity();
    if (!eye.valid || !yaw.valid || !pitch.valid) {
        return;
    }
    frame.world_live = true;
    frame.eye = eye.value;
    frame.yaw_degrees = yaw.value;
    frame.pitch_degrees = pitch.value;
    if (velocity.valid) {
        frame.velocity = velocity.value;
    }
#else
    (void)frame;
#endif
}

void build_hud_frame(hud::Frame& frame) noexcept {
    resolve_overlay_modules();
    const ImGuiIO& io = ImGui::GetIO();
    frame.screen = Rect{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y};
    frame.frames_per_second = hooks::game_thread::stats().frames_per_second;
    frame.alpha = 1.0f;
    // The frame's own delta: the Loyalty HUD's throw timer advances on this, so it follows the real
    // frame rate instead of assuming one (§7.4).
    frame.delta_seconds = io.DeltaTime;

    if (g_overlay.hud != nullptr && g_overlay.hud->enabled()) {
        frame.watermark = g_overlay.hud->watermark();
        frame.arraylist = g_overlay.hud->arraylist();
        frame.arraylist_by_length = g_overlay.hud->sort_by_length();
    }

    if (g_overlay.crosshair != nullptr && g_overlay.crosshair->enabled()) {
        frame.crosshair = true;
        frame.crosshair_style.shape = g_overlay.crosshair->shape();
        frame.crosshair_style.size = g_overlay.crosshair->size();
        frame.crosshair_style.gap = g_overlay.crosshair->gap();
        frame.crosshair_style.thickness = g_overlay.crosshair->thickness();
        frame.crosshair_style.color =
            CustomCrosshair::color_for(g_overlay.crosshair->color_index());
    }

    if (g_overlay.velocity != nullptr && g_overlay.velocity->enabled()) {
        frame.velocity_chip = true;
        frame.velocity_units = g_overlay.velocity->units_index();
        read_player_view(frame);
    }

    if (g_overlay.trajectories != nullptr && g_overlay.trajectories->enabled()) {
        frame.trajectory = true;
        frame.trajectory_style.params = g_overlay.trajectories->params();
        frame.trajectory_style.color =
            Trajectories::color_for(g_overlay.trajectories->color_index());
        read_player_view(frame);
    }

    // Step 8: the Combat/Mace readouts. Filled last so the modules that share the left-hand chip
    // column (reach, smash, counters) read the same frame the step-7 fields already populated.
    fill_step8_readouts(frame);
}

// Drawn after the chrome, so the HUD, crosshair and path sit above the game. It yields while the
// ClickGUI is open: the chrome is the focus then, and a crosshair drawn at its centre would
// otherwise land on top of the module cards.
void render_inworld_overlay() noexcept {
    if (util::clamp01(g_state.animation.value(g_state.open_spring)) > 0.001f) {
        return;
    }
    hud::Frame frame{};
    build_hud_frame(frame);
    if (!hud::active(frame)) {
        return;
    }
    hud::render(ImGui::GetForegroundDrawList(), frame);
}

// Mirrors an overlay-side visibility change (the red traffic light, focus loss) onto the ClickGUI
// module, so its card's pill can never disagree with the window (§6, ModuleCard's invariant).
void sync_clickgui_module() noexcept {
    modules::BaseModule* module = modules::manager().find("ClickGUI");
    if (module != nullptr && module->enabled() != g_state.visible) {
        module->set_enabled(g_state.visible);
        modules::manager().notify_changed();
    }
}

void compose_frame() noexcept {
    const float open = util::clamp01(g_state.animation.value(g_state.open_spring));
    const float collapse = util::clamp01(g_state.animation.value(g_state.collapse));

    // While the chrome is fully closed, only the toast stack may still need this frame (§7.8):
    // composing the window at alpha 0 would still emit every piece of geometry it owns.
    if (!g_state.visible && open <= 0.001f) {
        render_notifications_layer();
        return;
    }

    const Layout layout = compute_layout(open, collapse);
    const float alpha = 1.0f - collapse;
    const ImGuiIO& io = ImGui::GetIO();
    const Rect screen{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y};

    g_state.header = layout.header;
    g_state.density_button = Rect{layout.header.right() - kDensityButtonWidth - 16.0f,
        layout.header.center_y() - (kDensityButtonHeight * 0.5f), kDensityButtonWidth,
        kDensityButtonHeight};

    // ── Input phase, top-down (§7.3). Toasts are drawn last, so they are offered the click
    // first; then the chrome, the sidebar, the search field and finally the content pane.
    g_state.notifications.set_screen(screen);
    (void)g_state.notifications.handle_input(g_state.input);
    handle_lights(layout);
    handle_density_button();

    if (collapse < 0.999f) {
        sync_sidebar();
        g_state.sidebar.set_area(layout.rail);
        g_state.sidebar.set_alpha(alpha);
        (void)g_state.sidebar.handle_input(g_state.input);
        if (g_state.sidebar.take_selection_changed()) {
            g_state.selected = g_state.sidebar.selected();
            // Leaving a page closes its drawer and disarms its capture: both belong to a card that
            // is no longer on screen.
            g_state.expanded_module = nullptr;
            g_state.capture_index = -1;
            g_state.slider_drag = nullptr;
            g_state.list_dirty = true;
        }

        g_state.search.set_area(search_field_rect(layout.content));
        g_state.search.set_alpha(alpha);
        (void)g_state.search.handle_input(g_state.input);
        if (g_state.search.take_changed()) {
            g_state.list_dirty = true;
        }
        // The registry only changes at boot in this step, but the check is what keeps a future
        // dynamic registration from showing a stale list.
        if (modules::manager().count() != g_state.cached_module_count) {
            g_state.cached_module_count = modules::manager().count();
            g_state.list_dirty = true;
        }
        if (g_state.list_dirty) {
            recompute_visible_modules();
        }

        if (is_module_category(g_state.selected)) {
            handle_module_cards(card_pane(layout.content), alpha);
            handle_settings_rows();
        }
    }
    handle_drag(layout);

    // Component animate() only steers targets; the controller was ticked for this frame already,
    // and the GUI owns the single tick per frame (§7.4).
    g_state.sidebar.animate(0.0f);
    g_state.search.animate(0.0f);
    for (ModuleCard& card : g_state.cards) {
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
        g_state.sidebar.render(draw_list, layout.rail);
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
    if (!event.down) {
        return;
    }

    // An armed bind capture has priority over everything else: the next key is the user's new
    // bind, not a game input and not a module toggle. A repeat cannot re-arm anything, because the
    // first key-down already consumed the capture.
    if (g_state.capture_index >= 0) {
        event.consumed = true;
        if (event.repeat) {
            return;
        }
        const std::size_t index = static_cast<std::size_t>(g_state.capture_index);
        g_state.capture_index = -1;
        modules::BaseModule* module =
            index < modules::manager().count() ? modules::manager().at(index) : nullptr;
        if (module == nullptr) {
            return;
        }

        auto& badge = g_state.cards[index].badge();
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

    if (event.repeat || event.virtual_key != g_state.toggle_key) {
        return;
    }
    toggle();
    // The bind belongs to the GUI: consuming it keeps the game from also seeing RSHIFT.
    event.consumed = true;
}

void on_focus_event(events::FocusEvent& event) {
    if (event.focused || !g_state.visible) {
        return;
    }
    // macOS behaviour (§4.7): losing the window closes the ClickGUI. The red button and this share
    // one path, so "hidden" means exactly one thing.
    WOKE_LOG_DEBUG("gui: window lost focus - chrome hidden");
    set_visible(false);
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────────────

bool initialize() noexcept {
    if (g_state.initialized) {
        return true;
    }

    (void)::QueryPerformanceFrequency(&g_state.frequency);
    g_state.toggle_key = kDefaultToggleKey;
    g_state.selected = Section::Diagnostics;
    g_state.grid_layout = true;

    g_state.animation.reset();
    g_state.open_spring = g_state.animation.acquire_spring(0.0f);
    g_state.collapse = g_state.animation.acquire_state(0.0f, 12.0f);
    g_state.density = g_state.animation.acquire_state(0.0f, 16.0f);

    // Step 6: the widget library acquires every animation slot it will ever use here, once
    // (§7.4). A component handed a handle at boot cannot animate from a recycled one, and no
    // frame ever allocates a widget or a slot (§6.3). A ModuleCard owns five states (its hover,
    // its drawer reveal, the pill's t, and the badge's hover and capture pulse), which is what
    // sizes AnimationController::kMaxStates.
    g_state.lights.bind(g_state.animation);
    g_state.sidebar.bind(g_state.animation);
    g_state.search.bind(g_state.animation);
    g_state.notifications.bind(g_state.animation);
    for (ModuleCard& card : g_state.cards) {
        card.bind(g_state.animation);
    }

    g_state.context = ImGui::CreateContext();
    if (g_state.context == nullptr) {
        WOKE_LOG_ERROR("gui: ImGui context creation failed - the ClickGUI stays disabled");
        return false;
    }
    ImGui::SetCurrentContext(g_state.context);
    apply_imgui_style();

    g_state.key_subscription = events::bus().subscribe<events::KeyEvent, &on_key_event>();
    g_state.focus_subscription = events::bus().subscribe<events::FocusEvent, &on_focus_event>();

    g_state.initialized = true;
    WOKE_LOG_INFO("ui-theme: %zu sections, ImGui %s, toggle key 0x%02X", kSectionCount,
        IMGUI_VERSION, static_cast<unsigned int>(g_state.toggle_key));
    return true;
}

void shutdown() noexcept {
    if (!g_state.initialized) {
        return;
    }

    if (g_state.renderer_ready) {
        ImGui::SetCurrentContext(g_state.context);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplWin32_Shutdown();
        g_state.renderer_ready = false;
    }

    events::bus().unsubscribe(g_state.key_subscription);
    events::bus().unsubscribe(g_state.focus_subscription);
    g_state.key_subscription = events::Subscription{};
    g_state.focus_subscription = events::Subscription{};
    // The step-8 counter observer holds a bus slot of its own; released here so a hot unload can
    // never leave the bus calling into unmapped DLL code.
    shutdown_step8();

    // Reverse of initialize()'s bind order: every component returns its pooled slots, so a
    // re-injection (hot unload, then attach again) starts from a clean controller.
    for (ModuleCard& card : g_state.cards) {
        card.unbind();
    }
    g_state.notifications.unbind();
    g_state.search.unbind();
    g_state.sidebar.unbind();
    g_state.lights.unbind();

    if (g_state.context != nullptr) {
        ImGui::SetCurrentContext(g_state.context);
        ImGui::DestroyContext(g_state.context);
        g_state.context = nullptr;
    }

    hooks::game_thread::set_overlay_requested(false);
    WOKE_LOG_INFO("gui: shutdown (%zu frame(s) rendered, %zu suppressed)", g_state.frames_rendered,
        g_state.suppressed_frames);
    g_state.initialized = false;
}

void attach_window(void* hwnd) noexcept {
    g_state.window = static_cast<HWND>(hwnd);
    if (g_state.window != nullptr) {
        g_state.window_positioned = false;
    }
}

bool handle_window_message(
    void* hwnd, unsigned int message, unsigned long long wparam, long long lparam) noexcept {
    if (!g_state.initialized || g_state.context == nullptr || !g_state.visible) {
        return false;
    }

    ImGui::SetCurrentContext(g_state.context);
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
    case WM_MOUSEHWHEEL:
        // Clicks and wheel are the GUI's while it is visible; the game must not mine, attack or
        // change hotbar slots behind it.
        return true;

    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        // The toggle bind has to reach the event bus, which is what closes the GUI again.
        return static_cast<int>(wparam) != g_state.toggle_key;

    case WM_CHAR:
    case WM_SYSCHAR:
        return true; // typed characters must not reach the game's chat or inventory

    default:
        return false;
    }
}

void render_overlay() noexcept {
    if (!g_state.initialized) {
        return;
    }

    // §7.8. While hidden and settled this is the entire cost of a frame: one branch. No NewFrame,
    // no vertex generation, no draw-list traversal.
    if (!needs_render()) {
        ++g_state.suppressed_frames;
        if (!g_state.suppression_logged) {
            g_state.suppression_logged = true;
            WOKE_LOG_INFO(
                "gui-renderer: chrome hidden after %zu frame(s) - frames now return before ImGui "
                "(%zu suppressed total, zero draw calls)",
                g_state.frames_rendered, g_state.suppressed_frames);
        }
        return;
    }

    if (!ensure_renderer()) {
        // No window yet or no backend: count the frame as suppressed, because no ImGui work
        // happened, and try again next frame.
        ++g_state.suppressed_frames;
        return;
    }

    if (ImGui::GetCurrentContext() != g_state.context) {
        ImGui::SetCurrentContext(g_state.context);
    }

    LARGE_INTEGER start{};
    (void)::QueryPerformanceCounter(&start);

    ImGui_ImplWin32_NewFrame();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    g_state.input = snapshot_input();
    const double now = now_seconds();
    tick_animation(now);
    refresh_readouts(now);
    compose_frame();

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    LARGE_INTEGER end{};
    (void)::QueryPerformanceCounter(&end);
    const float chrome_ms = milliseconds_between(start, end);

    ++g_state.frames_rendered;
    g_state.last_chrome_ms = chrome_ms;
    g_state.suppression_logged = false;
    if (g_state.frames_rendered == 1) {
        g_state.average_chrome_ms = chrome_ms;
    } else {
        g_state.average_chrome_ms += (chrome_ms - g_state.average_chrome_ms) * 0.05f;
    }
    if (chrome_ms > g_state.worst_chrome_ms) {
        g_state.worst_chrome_ms = chrome_ms;
    }

    if (g_state.average_chrome_ms > kChromeBudgetMs
        && (now - g_state.last_chrome_warning_seconds) > kChromeWarningCooldownSeconds) {
        g_state.last_chrome_warning_seconds = now;
        WOKE_LOG_WARN("gui-renderer: chrome cost %.3f ms exceeds the %.2f ms budget (worst %.3f ms)",
            static_cast<double>(g_state.average_chrome_ms), static_cast<double>(kChromeBudgetMs),
            static_cast<double>(g_state.worst_chrome_ms));
    }

    if (g_state.frames_rendered % kHeartbeatFrames == 0) {
        WOKE_LOG_DEBUG("gui-renderer: %zu frames, chrome avg %.3f ms, open %.2f, %zu anim slot(s)",
            g_state.frames_rendered, static_cast<double>(g_state.average_chrome_ms),
            static_cast<double>(util::clamp01(g_state.animation.value(g_state.open_spring))),
            g_state.animation.active_state_count());
    }
}

bool visible() noexcept {
    return g_state.visible;
}

void set_visible(bool requested) noexcept {
    if (g_state.visible == requested) {
        return;
    }
    g_state.visible = requested;
    if (requested) {
        // Snap the pointer state so the first frame cannot see a stale click, and clear the
        // suppression counter for the next close.
        g_state.suppression_logged = true;
        WOKE_LOG_INFO("gui: chrome shown (%zu section(s), grid=%s)",
            kSectionCount, g_state.grid_layout ? "yes" : "no");
    }
    // The frame scheduler's suppression predicate reads this (§7.8): while the chrome is visible
    // the pipeline is known to have work, independent of who is subscribed.
    hooks::game_thread::set_overlay_requested(requested);
}

void toggle() noexcept {
    set_visible(!g_state.visible);
}

bool needs_render() noexcept {
    if (g_state.visible) {
        return true;
    }
    // Still closing: the close animation has to be rendered or it never finishes.
    if (!g_state.animation.at_rest(g_state.open_spring)) {
        return true;
    }
    // A toast outlives the chrome: the ClickGUI can be hidden while one is still on screen, and
    // that frame must not be suppressed or the toast would freeze mid-slide (§7.5, §7.8).
    if (g_state.notifications.needs_render()) {
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
    return g_state.toggle_key;
}

void set_toggle_key(int virtual_key) noexcept {
    if (virtual_key > 0) {
        g_state.toggle_key = virtual_key;
    }
}

void notify(const char* title, const char* message, bool warning) noexcept {
    if (!g_state.initialized) {
        return;
    }
    push_toast(title != nullptr ? title : version::kClientName,
        message != nullptr ? message : "", warning ? ToastIcon::Warning : ToastIcon::Info);
}

void notify_keybind_toggle(int virtual_key) noexcept {
    if (!g_state.initialized || virtual_key == 0) {
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
    Stats result;
    result.initialized = g_state.initialized;
    result.renderer_ready = g_state.renderer_ready;
    result.visible = g_state.visible;
    result.collapsed = g_state.collapsed;
    result.suppressed = !needs_render();
    result.frames_rendered = g_state.frames_rendered;
    result.suppressed_frames = g_state.suppressed_frames;
    result.open_amount = util::clamp01(g_state.animation.value(g_state.open_spring));
    result.last_chrome_ms = g_state.last_chrome_ms;
    result.average_chrome_ms = g_state.average_chrome_ms;
    result.worst_chrome_ms = g_state.worst_chrome_ms;
    result.animation_slots = g_state.animation.active_state_count()
        + g_state.animation.active_spring_count();
    result.animation_overflows = g_state.animation.overflow_count();
    return result;
}

} // namespace woke::ui
