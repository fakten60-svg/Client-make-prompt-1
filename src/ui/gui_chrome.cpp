#include "ui/gui_internal.h"

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include <imgui.h>

#include "core/build_config.h"
#include "core/event_bus.h"
#include "core/logger.h"
#include "core/events.h"
#include "core/perf.h"
#include "core/version.h"
#include "hooks/game_thread.h"
#include "hooks/hook_manager.h"
#include "hooks/swap_hook.h"
#include "hooks/wndproc_hook.h"
#include "modules/category.h"
#include "modules/module_manager.h"
#include "ui/animation/easing.h"
#include "ui/components/keybind_badge.h"
#include "ui/hud.h"
#include "ui/overlay.h"
#include "ui/step8_frame.h"

#if WOKE_HAVE_JNI
#include "jni/reflection_cache.h"
#endif

// The ClickGUI's static composition (blueprint §7.1-§7.5, roadmap steps 4-6).
//
// Timing, the ImGui theme, the Diagnostics readouts, layout geometry, the widget interaction
// passes and every drawing routine. The per-frame orchestration lives in gui_overlay.cpp and the
// in-world overlay in gui_hud.cpp; see gui_internal.h for the contract between the three.

namespace woke::ui {
namespace detail {

using draw::Align;
using ToastIcon = woke::ui::ToastIcon; // notifications.h declares it in woke::ui directly

// ── The one state instance ─────────────────────────────────────────────────────────

State& state() noexcept {
    static State instance;
    return instance;
}

// ── Timing ────────────────────────────────────────────────────────────────────────

double now_seconds() noexcept {
    const State& s = state();
    if (s.frequency.QuadPart <= 0) {
        return 0.0;
    }
    LARGE_INTEGER counter{};
    (void)::QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(s.frequency.QuadPart);
}

float milliseconds_between(const LARGE_INTEGER& start, const LARGE_INTEGER& end) noexcept {
    const State& s = state();
    if (s.frequency.QuadPart <= 0) {
        return 0.0f;
    }
    const double ticks = static_cast<double>(end.QuadPart - start.QuadPart);
    return static_cast<float>((ticks * 1000.0) / static_cast<double>(s.frequency.QuadPart));
}

// ── Theme application ─────────────────────────────────────────────────────────────

void apply_imgui_style() noexcept {
    const theme::StyleTokens tokens = theme::style_tokens();
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding = ImVec2(tokens.window_padding, tokens.window_padding);
    style.FramePadding = ImVec2(tokens.frame_padding_x, tokens.frame_padding_y);
    style.ItemSpacing = ImVec2(tokens.item_spacing_x, tokens.item_spacing_y);
    style.WindowRounding = tokens.window_rounding;
    style.FrameRounding = tokens.frame_rounding;
    style.WindowBorderSize = tokens.window_border_size;
    style.ScrollbarSize = tokens.scrollbar_size;
    style.GrabRounding = tokens.grab_rounding;
    style.TabRounding = tokens.tab_rounding;

    // The overlay is drawn at the game's resolution, so the default 13 px font is scaled up once
    // here rather than per text call.
    style.FontScaleMain = 1.1f;

    style.Colors[ImGuiCol_WindowBg] = draw::to_im_vec4(theme::color::kWindowBackdrop);
    style.Colors[ImGuiCol_Border] = draw::to_im_vec4(theme::color::kWindowBorder);
    style.Colors[ImGuiCol_Text] = draw::to_im_vec4(theme::color::kText);
    style.Colors[ImGuiCol_TextDisabled] = draw::to_im_vec4(theme::color::kTextDim);
    style.Colors[ImGuiCol_Separator] = draw::to_im_vec4(theme::color::kSeparator);
    style.Colors[ImGuiCol_ScrollbarBg] = draw::to_im_vec4(util::kTransparent);
    style.Colors[ImGuiCol_ScrollbarGrab] = draw::to_im_vec4(theme::color::kCardBorder);
    style.Colors[ImGuiCol_FrameBg] = draw::to_im_vec4(theme::color::kCard);
    style.Colors[ImGuiCol_FrameBgHovered] = draw::to_im_vec4(theme::color::kCardHover);
    style.Colors[ImGuiCol_CheckMark] = draw::to_im_vec4(theme::color::kAccent);
    style.Colors[ImGuiCol_SliderGrab] = draw::to_im_vec4(theme::color::kAccent);
    style.Colors[ImGuiCol_Button] = draw::to_im_vec4(theme::color::kCard);
    style.Colors[ImGuiCol_ButtonHovered] = draw::to_im_vec4(theme::color::kCardHover);
    style.Colors[ImGuiCol_ButtonActive] = draw::to_im_vec4(theme::color::kAccent);
}

// ── Readouts ──────────────────────────────────────────────────────────────────────

const char* overlay_state_text() noexcept {
    const State& s = state();
    if (s.renderer_failed) {
        return "backend unavailable";
    }
    if (!s.visible) {
        return "hidden - zero draw calls";
    }
    if (!s.renderer_ready) {
        return "waiting for the game window";
    }
    return "rendering";
}

void refresh_readouts(double now) noexcept {
    State& s = state();
    if (now < s.readout_refresh_at) {
        return;
    }
    s.readout_refresh_at = now + kReadoutIntervalSeconds;

    const hooks::game_thread::Stats pipeline = hooks::game_thread::stats();

    set_readout(0, "Frame rate", "%.1f fps", static_cast<double>(pipeline.frames_per_second));
    set_readout(1, "Frames / ticks", "%llu / %llu",
        static_cast<unsigned long long>(pipeline.frames),
        static_cast<unsigned long long>(pipeline.ticks));
    set_readout(2, "Pipeline cost", "avg %.3f ms",
        static_cast<double>(pipeline.average_pipeline_ms));
    set_readout(3, "Chrome cost", "avg %.3f ms", static_cast<double>(s.average_chrome_ms));
    set_readout(4, "Swap hook", "%llu frames",
        static_cast<unsigned long long>(hooks::swap_hook_frame_count()));
    set_readout(5, "Hooks", "%zu active, %zu queued", hooks::active_hook_count(),
        hooks::queued_removal_count());
    set_readout(6, "Window input", "%llu messages",
        static_cast<unsigned long long>(hooks::routed_message_count()));
#if WOKE_HAVE_JNI
    const jni::CacheStats cache = jni::stats();
    set_readout(7, "JVM bridge", "%zu class, %zu method", cache.classes, cache.methods);
#else
    set_readout(7, "JVM bridge", "%s", "not built (no JDK headers)");
#endif
    set_readout(8, "Overlay", "%s", overlay_state_text());
    set_readout(9, "Target", "%s %s", version::kTargetGame, version::kVersion);
}

// ── Geometry ──────────────────────────────────────────────────────────────────────

Layout compute_layout(float open_amount, float collapse) noexcept {
    State& s = state();
    const ImGuiIO& io = ImGui::GetIO();
    const float scale =
        util::lerp(theme::metrics::kOpenScaleFrom, 1.0f, animation::ease_out_expo(open_amount));

    const float width = util::lerp(theme::metrics::kWindowWidth, theme::metrics::kCollapsedWidth,
                            collapse)
        * scale;
    const float height = util::lerp(theme::metrics::kWindowHeight,
                             theme::metrics::kCollapsedHeight, collapse)
        * scale;

    if (!s.window_positioned) {
        s.window_position = ImVec2((io.DisplaySize.x - width) * 0.5f,
            (io.DisplaySize.y - height) * 0.5f);
        s.window_positioned = true;
    }

    // Keep the window reachable even if the display was resized under it.
    const float max_x = io.DisplaySize.x > width ? io.DisplaySize.x - width : 0.0f;
    const float max_y = io.DisplaySize.y > height ? io.DisplaySize.y - height : 0.0f;
    s.window_position.x = util::clamp(s.window_position.x, 0.0f, max_x);
    s.window_position.y = util::clamp(s.window_position.y, 0.0f, max_y);

    Layout layout;
    layout.window = Rect{s.window_position.x, s.window_position.y, width, height};
    layout.header = layout.window.slice_top(util::lerp(theme::metrics::kHeaderHeight,
        theme::metrics::kCollapsedHeight, collapse));
    layout.body = layout.window.without_top(layout.header.h);
    const float sidebar = util::lerp(theme::metrics::kSidebarWidth, 0.0f, collapse);
    layout.rail = layout.body.slice_left(sidebar);
    layout.content = layout.body.without_left(sidebar);
    return layout;
}

Input snapshot_input() noexcept {
    const ImGuiIO& io = ImGui::GetIO();
    Input input;
    input.mouse_x = io.MousePos.x;
    input.mouse_y = io.MousePos.y;
    for (std::size_t button = 0; button < input.down.size(); ++button) {
        const int index = static_cast<int>(button);
        input.down[button] = io.MouseDown[index];
        input.pressed[button] = ImGui::IsMouseClicked(index);
        input.released[button] = ImGui::IsMouseReleased(index);
    }
    input.wheel = io.MouseWheel;
    return input;
}

// ── Interaction ───────────────────────────────────────────────────────────────────

void handle_drag(const Layout& layout) noexcept {
    State& s = state();
    const Input& input = s.input;

    if (s.dragging) {
        if (input.is_down(0)) {
            s.window_position.x = input.mouse_x - s.drag_offset_x;
            s.window_position.y = input.mouse_y - s.drag_offset_y;
            s.window_positioned = true;
        } else {
            s.dragging = false;
        }
        return;
    }

    // A drag starts on the chrome bar, but not on a traffic light or the density button: those
    // consume their own press, and the header is otherwise the window's grab handle.
    if (!input.is_pressed(0) || s.collapsed) {
        return;
    }
    if (!layout.header.contains(input.mouse_x, input.mouse_y)) {
        return;
    }
    if (s.density_button.contains(input.mouse_x, input.mouse_y)) {
        return;
    }

    const Rect lights = Rect{layout.header.left() + theme::metrics::kTrafficLightInset,
        layout.header.top(), theme::metrics::kTrafficLightGap * 3.0f, layout.header.h};
    for (std::size_t index = 0; index < TrafficLights::kCount; ++index) {
        if (s.lights.button_rect(lights, index).inset(-4.0f).contains(input.mouse_x,
                input.mouse_y)) {
            return;
        }
    }

    s.dragging = true;
    s.drag_offset_x = input.mouse_x - s.window_position.x;
    s.drag_offset_y = input.mouse_y - s.window_position.y;
}

// Pushes the registry's counters and the frame rate into the sidebar. Two int reads per category
// by design (§4.4): the manager precomputes the buckets, so this is six lookups, not a scan.
void sync_sidebar() noexcept {
    State& s = state();
    const modules::ModuleManager& registry = modules::manager();
    for (std::size_t index = 0; index < kModuleCategoryCount; ++index) {
        const modules::Category category =
            modules::category_from_section(static_cast<Section>(index));
        s.sidebar.set_counts(index, registry.category_enabled(category),
            registry.category_total(category));
    }

    const hooks::game_thread::Stats pipeline = hooks::game_thread::stats();
    FixedString<64> footer;
    footer.format("%s %s | %.0f fps", version::kClientName, version::kVersion,
        static_cast<double>(pipeline.frames_per_second));
    s.sidebar.set_footer(footer.c_str());
}

Rect content_body(const Rect& content) noexcept {
    return content.inset(theme::metrics::kContentPadding);
}

Rect search_field_rect(const Rect& content) noexcept {
    const Rect body = content_body(content);
    return Rect{body.right() - kSearchWidth, body.top() - 2.0f, kSearchWidth, kSearchHeight};
}

Rect card_pane(const Rect& content) noexcept {
    const Rect body = content_body(content);
    const float header_drop = kTileHeaderOffset + theme::metrics::kHeaderFontSize + 6.0f;
    return Rect{body.left(), body.top() + header_drop, body.w,
        body.h > header_drop ? body.h - header_drop : 0.0f};
}

void handle_lights(const Layout& layout) noexcept {
    State& s = state();
    const Rect lights = Rect{layout.header.left() + theme::metrics::kTrafficLightInset,
        layout.header.top(), theme::metrics::kTrafficLightGap * 3.0f, layout.header.h};
    s.lights.set_area(lights);
    (void)s.lights.handle_input(s.input);

    switch (s.lights.take_action()) {
    case TrafficLights::Action::Close:
        // Red hides the GUI; it never unloads the client (§3.6).
        set_visible(false);
        break;
    case TrafficLights::Action::Minimize:
        // Yellow collapses to a pill that stays clickable, so the window can be brought back
        // without a keybind.
        s.collapsed = !s.collapsed;
        s.window_positioned = true;
        break;
    case TrafficLights::Action::Zoom:
        // Green toggles the content density: grid <-> list, the same axis the module grid uses.
        s.grid_layout = !s.grid_layout;
        s.animation.set_target(s.density, s.grid_layout ? 0.0f : 1.0f);
        break;
    case TrafficLights::Action::None:
        break;
    }
}

void handle_density_button() noexcept {
    State& s = state();
    if (!s.density_button.contains(s.input.mouse_x, s.input.mouse_y)) {
        return;
    }
    if (s.input.is_pressed(0)) {
        s.grid_layout = !s.grid_layout;
        s.animation.set_target(s.density, s.grid_layout ? 0.0f : 1.0f);
    }
}

// ── Toasts and the overlay-awake flag ───────────────────────────────────────────────

void sync_clickgui_module() noexcept {
    modules::BaseModule* module = modules::manager().find("ClickGUI");
    if (module != nullptr && module->enabled() != state().visible) {
        module->set_enabled(state().visible);
        modules::manager().notify_changed();
    }
}

void sync_overlay_request() noexcept {
    // The ClickGUI module mirrors visibility whichever way it changed: a card click, a keybind, or
    // the overlay's own red traffic light.
    sync_clickgui_module();
    hooks::game_thread::set_overlay_requested(
        state().visible || state().notifications.needs_render() || inworld_overlay_wanted());
}

void push_toast(const char* title, const char* message, ToastIcon icon) noexcept {
    State& s = state();
    Toast toast;
    toast.title = title;
    toast.message = message;
    toast.icon = icon;
    if (!s.notifications.push(toast)) {
        WOKE_LOG_DEBUG("notifications: pool full - a toast was dropped");
        return;
    }
    sync_overlay_request();
}

// Every module state change is user-visible (§4.4), whether it came from a card click or a
// keybind: this is the one place the toggle toast is composed.
void push_module_toast(const char* name, bool enabled) noexcept {
    push_toast(name != nullptr ? name : "Module", enabled ? "Enabled" : "Disabled",
        enabled ? ToastIcon::Success : ToastIcon::Info);
}

// ── Module cards and settings rows ──────────────────────────────────────────────────

// Rebuilds the filtered module list for the selected category, and the header's counter line with
// it. Runs on a query change or a registry change - never on a timer (§7.1).
void recompute_visible_modules() noexcept {
    State& s = state();
    const modules::ModuleManager& registry = modules::manager();
    const modules::Category category = modules::category_from_section(s.selected);
    std::size_t count = 0;
    for (std::size_t index = 0; index < registry.count(); ++index) {
        modules::BaseModule* module = registry.at(index);
        if (module == nullptr || module->category() != category) {
            continue;
        }
        // A query matches the name or the description: "zoom" should find Zoom, and "sprint"
        // should also find the module that is *described* as a sprint helper.
        if (!s.search.empty() && !s.search.matches(module->name())
            && !s.search.matches(module->description())) {
            continue;
        }
        s.visible_modules[count] = module;
        s.visible_indices[count] = index;
        ++count;
    }
    s.visible_count = count;
    s.list_dirty = false;

    // Formatted here rather than per frame: the numbers move only when the list does (§7.1).
    s.module_counts.format("%zu/%zu shown  |  %zu enabled", s.visible_count,
        registry.category_total(category), registry.category_enabled(category));
}

// Card rectangles for the visible modules, laid out exactly as draw_module_cards places them, so
// a click can never land on a card the frame has not drawn. Grid density puts two half-width
// tiles per row; list density - and an *expanded* card - takes the full width, because a drawer
// needs the room and a half-width drawer would clip its own rows.
void layout_module_cards(const Rect& pane) noexcept {
    State& s = state();
    const float density = util::clamp01(s.animation.value(s.density));
    const float gap = theme::metrics::kTileGap;
    const float half_width = (pane.w - gap) * 0.5f;
    const float body = ModuleCard::body_height(density);
    const bool list_mode = density >= 0.999f;

    float x = pane.left();
    float y = pane.top();
    bool right_column = false;
    float row_height = 0.0f;
    std::size_t placed = 0;

    for (std::size_t index = 0; index < s.visible_count; ++index) {
        modules::BaseModule* module = s.visible_modules[index];
        const bool expanded = (module == s.expanded_module);
        const float height =
            body + (expanded ? ModuleCard::drawer_height_for(module->setting_count()) : 0.0f);
        const float width = (list_mode || expanded) ? pane.w : half_width;

        if ((list_mode || expanded) && right_column) {
            // A full-width card cannot share a row: close the half-filled one first.
            y += row_height + gap;
            x = pane.left();
            right_column = false;
            row_height = 0.0f;
        }
        if (y + height > pane.bottom() + 0.5f) {
            break; // no scrolling yet: the pane clips, exactly like the step-4 readout tiles
        }

        s.card_rects[placed] = Rect{x, y, width, height};
        s.card_registry[placed] = s.visible_indices[index];
        ++placed;

        if (width >= pane.w - 0.001f) {
            y += height + gap;
            x = pane.left();
            right_column = false;
            row_height = 0.0f;
        } else if (!right_column) {
            right_column = true;
            x = pane.left() + width + gap;
            row_height = height;
        } else {
            y += row_height + gap;
            x = pane.left();
            right_column = false;
            row_height = 0.0f;
        }
    }
    s.card_count = placed;
}

// The card currently carrying an open drawer, by layout slot. kMaxModules means "none".
std::size_t expanded_slot() noexcept {
    const State& s = state();
    if (s.expanded_module == nullptr) {
        return modules::ModuleManager::kMaxModules;
    }
    for (std::size_t slot = 0; slot < s.card_count; ++slot) {
        if (modules::manager().at(s.card_registry[slot]) == s.expanded_module) {
            return slot;
        }
    }
    return modules::ModuleManager::kMaxModules;
}

// Where a settings row sits inside a drawer. The *card* owns the drawer shell; its contents are
// the GUI's, because the GUI is the layer that knows about settings (ModuleCard stays independent
// of the module/setting model, which is what lets the host tests drive it standalone).
Rect setting_row_rect(const Rect& drawer, std::size_t row) noexcept {
    return Rect{drawer.left() + kSettingRowInset,
        drawer.top() + ModuleCard::kDrawerPadding
            + (static_cast<float>(row) * ModuleCard::kDrawerRowHeight),
        drawer.w - (kSettingRowInset * 2.0f), ModuleCard::kDrawerRowHeight};
}

// The open drawer's rectangle in this frame's layout; empty when nothing is expanded or the card
// fell outside the visible pane.
Rect expanded_drawer_rect() noexcept {
    const State& s = state();
    const std::size_t slot = expanded_slot();
    if (slot >= s.card_count) {
        return Rect{};
    }
    const std::size_t registry_index = s.card_registry[slot];
    const modules::BaseModule* module = modules::manager().at(registry_index);
    if (module == nullptr) {
        return Rect{};
    }
    return ModuleCard::drawer_area_for(s.card_rects[slot], module->setting_count());
}

void handle_module_cards(const Rect& pane, float alpha) noexcept {
    State& s = state();
    layout_module_cards(pane);

    const Input& input = s.input;
    for (std::size_t slot = 0; slot < s.card_count; ++slot) {
        const std::size_t registry_index = s.card_registry[slot];
        modules::BaseModule* module = modules::manager().at(registry_index);
        if (module == nullptr) {
            continue;
        }

        ModuleCard& card = s.cards[registry_index];
        card.set_area(s.card_rects[slot]);
        card.set_alpha(alpha);
        card.set_content(module->name(), module->description(), module->bind());
        card.set_enabled(module->enabled());
        card.set_expanded(module == s.expanded_module);
        card.set_drawer_rows(module->setting_count());
        // Exactly one card can be the capture target, because there is one capture index.
        card.set_capture_armed(s.capture_index == static_cast<int>(registry_index));

        // All mutation happens in this input phase (§7.3: render never mutates); the draw phase
        // only reads the resulting state.
        const ModuleCard::Result result = card.interact(input);
        if (result.bind_clicked) {
            const bool arming = s.capture_index != static_cast<int>(registry_index);
            s.capture_index = arming ? static_cast<int>(registry_index) : -1;
            if (arming) {
                WOKE_LOG_DEBUG("keybind: capturing a new bind for '%s'", module->name());
            }
            continue; // a chip click is never also a toggle
        }
        if (result.expand_toggled) {
            // One drawer at a time: a second open drawer would push the card being read off the
            // pane, and this layout has no scrolling yet.
            s.expanded_module = (s.expanded_module == module) ? nullptr : module;
            continue;
        }
        if (result.toggled && module->set_enabled(!module->enabled())) {
            modules::manager().notify_changed();
            (void)save_config();
            card.set_enabled(module->enabled()); // the pill must not lag the click by a frame
            s.list_dirty = true;                 // the sidebar's counter badges changed
            push_module_toast(module->name(), module->enabled());
        }
    }
}

// Arms the card's keybind capture for a module. Exactly one capture exists at a time and the card's
// badge owns the state machine, so a rebind started from a drawer row and one started from the
// chip are the same code path - which is what keeps the two affordances from disagreeing about the
// current bind. The write itself goes through BaseModule::set_bind(), whose value is a persisted
// BindSetting, so a rebind from either place survives the session.
void arm_bind_capture(modules::BaseModule* module) noexcept {
    const modules::ModuleManager& registry = modules::manager();
    for (std::size_t index = 0; index < registry.count(); ++index) {
        if (registry.at(index) == module) {
            state().capture_index = static_cast<int>(index);
            WOKE_LOG_DEBUG("keybind: capturing a new bind for '%s'", module->name());
            return;
        }
    }
}

// The one place a setting is edited from the overlay. Clicking a bool flips it, clicking an enum
// cycles it, and a slider follows the pointer for as long as the button is held - including after
// the pointer leaves the row, because "you must keep the cursor inside" is the classic way for a
// slider to feel broken.
void handle_settings_rows() noexcept {
    State& s = state();
    modules::BaseModule* module = s.expanded_module;
    if (module == nullptr) {
        s.slider_drag = nullptr;
        return;
    }
    const Rect drawer = expanded_drawer_rect();
    if (drawer.empty()) {
        return;
    }

    const Input& input = s.input;
    for (std::size_t row = 0; row < module->setting_count(); ++row) {
        settings::Setting* setting = module->settings()[row];
        if (setting == nullptr) {
            continue;
        }
        const Rect rect = setting_row_rect(drawer, row);
        const bool hovered = rect.contains(input.mouse_x, input.mouse_y);

        if (setting->kind() == settings::Kind::Slider) {
            if (hovered && input.is_pressed(0)) {
                s.slider_drag = setting;
            }
            if (s.slider_drag == setting) {
                if (input.is_down(0)) {
                    const float t = util::inverse_lerp(rect.left(), rect.right(), input.mouse_x);
                    setting->set_float(
                        util::lerp(setting->float_minimum(), setting->float_maximum(), t));
                } else {
                    s.slider_drag = nullptr;
                    (void)save_config();
                }
            }
            continue;
        }

        // A bind row arms capture rather than editing in place: the value is "the next key you
        // press", and the key has to arrive through the window procedure.
        if (setting->kind() == settings::Kind::Bind) {
            if (hovered && input.is_pressed(0)) {
                arm_bind_capture(module);
            }
            continue;
        }

        if (!hovered || !input.is_pressed(0)) {
            continue;
        }
        if (setting->kind() == settings::Kind::Bool) {
            setting->set_bool(!setting->bool_value());
        } else if (setting->kind() == settings::Kind::Enum && setting->enum_count() > 0) {
            setting->set_enum_index((setting->enum_index() + 1) % setting->enum_count());
        }
        (void)save_config();
    }
}

void draw_settings_rows(ImDrawList* draw_list, float alpha) noexcept {
    const State& s = state();
    const std::size_t slot = expanded_slot();
    const modules::BaseModule* module = s.expanded_module;
    if (module == nullptr || slot >= s.card_count || module->setting_count() == 0) {
        return;
    }
    // The rows fade with the drawer's own reveal, so they can never be visible through a closed
    // drawer shell.
    const float open = s.cards[s.card_registry[slot]].open_amount();
    if (open <= 0.01f) {
        return;
    }
    const Rect drawer =
        ModuleCard::drawer_area_for(s.card_rects[slot], module->setting_count());
    const float row_alpha = alpha * open;
    const Input& input = s.input;

    for (std::size_t row = 0; row < module->setting_count(); ++row) {
        const settings::Setting* setting = module->settings()[row];
        if (setting == nullptr) {
            continue;
        }
        const Rect rect = setting_row_rect(drawer, row);
        const bool active = rect.contains(input.mouse_x, input.mouse_y)
            || s.slider_drag == setting;

        if (active) {
            draw::rounded_rect(draw_list, rect,
                util::with_alpha(theme::color::kCardHover, row_alpha * 0.6f),
                theme::metrics::kFrameRounding);
        }

        const float label_width = rect.w - kSettingValueColumn - 16.0f;
        draw::text_clipped(draw_list, Rect{rect.left() + 8.0f, rect.top(), label_width, rect.h},
            setting->name(), util::with_alpha(theme::color::kTextMuted, row_alpha), Align::Left,
            theme::metrics::kFooterFontSize);

        FixedString<24> value;
        switch (setting->kind()) {
        case settings::Kind::Bool:
            value.assign(setting->bool_value() ? "on" : "off");
            break;
        case settings::Kind::Slider:
            value.format("%.2f", static_cast<double>(setting->float_value()));
            break;
        case settings::Kind::Enum:
            value.assign(setting->enum_label());
            break;
        case settings::Kind::Bind: {
            // The same key-name table the card's chip uses, so a bind reads identically in both
            // places (§3.6: one vocabulary for "which key is this").
            char key_label[16];
            components::format_key(setting->bind_value(), key_label, sizeof(key_label));
            value.assign(key_label);
            break;
        }
        default:
            break;
        }

        // An armed bind and a lit bool share the accent treatment: both mean "this is set" rather
        // than "this is at its default".
        const bool is_live = (setting->kind() == settings::Kind::Bool && setting->bool_value())
            || (setting->kind() == settings::Kind::Bind && setting->bind_value() != 0);
        const Rgba value_color = is_live ? theme::color::kAccent : theme::color::kText;
        draw::text_clipped(draw_list,
            Rect{rect.right() - kSettingValueColumn, rect.top(), kSettingValueColumn - 8.0f,
                rect.h},
            value.c_str(), util::with_alpha(value_color, row_alpha), Align::Right,
            theme::metrics::kFooterFontSize);

        if (setting->kind() == settings::Kind::Slider) {
            // A track along the row's lower edge: the fill's width *is* the value, so the drawer
            // looks interactive without a second widget to explain it.
            const float span = setting->float_maximum() - setting->float_minimum();
            const float t = span > 0.0f
                ? util::clamp01((setting->float_value() - setting->float_minimum()) / span)
                : 0.0f;
            const Rect track{rect.left() + 8.0f, rect.bottom() - 5.0f, rect.w - 16.0f, 3.0f};
            draw::rounded_rect(draw_list, track,
                util::with_alpha(theme::color::kCardBorder, row_alpha), 1.5f);
            if (t > 0.002f) {
                draw::rounded_rect(draw_list, Rect{track.left(), track.top(), track.w * t, track.h},
                    util::with_alpha(theme::color::kAccent, row_alpha), 1.5f);
            }
        }
    }
}

// Draw-only pass: the cards were handed this frame's alpha in the input phase
// (handle_module_cards sets it before interacting), so this function takes no alpha of its own.
void draw_module_cards(ImDrawList* draw_list) noexcept {
    State& s = state();
    for (std::size_t slot = 0; slot < s.card_count; ++slot) {
        const std::size_t registry_index = s.card_registry[slot];
        const Rect& card = s.card_rects[slot];
        if (card.empty()) {
            continue;
        }
        // No state is mutated here: the rectangles, the alpha and every target were set in this
        // frame's input phase, and the card renders itself from them (§7.3).
        s.cards[registry_index].render(draw_list, card);
    }
}

// ── Drawing ───────────────────────────────────────────────────────────────────────

FixedString<64> compose_title() noexcept {
    const State& s = state();
    FixedString<64> title;
    title.assign(version::kClientName);
    if (!s.collapsed) {
        title.append("  ·  ");
        title.append(section_label(s.selected));
    }
    return title;
}

void draw_chrome_bar(ImDrawList* draw_list, const Layout& layout, float collapse) noexcept {
    State& s = state();
    const Rgba chrome = util::with_alpha(theme::color::kChromeBar, 1.0f - (0.6f * collapse));
    draw::rounded_rect(draw_list, layout.header, chrome, theme::metrics::kWindowRounding,
        ImDrawFlags_RoundCornersTop);

    s.lights.render(draw_list, Rect{layout.header.left() + theme::metrics::kTrafficLightInset,
                                    layout.header.top(), theme::metrics::kTrafficLightGap * 3.0f,
                                    layout.header.h});

    const FixedString<64> title = compose_title();
    draw::text_in(draw_list, layout.header, title.c_str(), theme::color::kText, Align::Center,
        theme::metrics::kHeaderFontSize);

    if (collapse < 0.5f) {
        const char* density = s.grid_layout ? "Grid" : "List";
        draw::rounded_rect(draw_list, s.density_button, theme::color::kCard,
            theme::metrics::kFrameRounding);
        draw::border_stroke(draw_list, s.density_button, theme::color::kCardBorder,
            theme::metrics::kFrameRounding);
        draw::text_in(draw_list, s.density_button, density, theme::color::kTextMuted,
            Align::Center);
    }

    draw::line(draw_list, layout.header.left(), layout.header.bottom(), layout.header.right(),
        layout.header.bottom(), theme::color::kSeparator, theme::metrics::kSeparatorThickness);
}

void draw_tile(ImDrawList* draw_list, const Rect& tile, const Readout& readout, float alpha) noexcept {
    draw::rounded_rect(draw_list, tile, util::with_alpha(theme::color::kCard, alpha),
        theme::metrics::kFrameRounding);
    draw::border_stroke(draw_list, tile, util::with_alpha(theme::color::kCardBorder, alpha),
        theme::metrics::kFrameRounding);

    const Rect label_area{tile.left() + theme::metrics::kContentPadding * 0.6f,
        tile.top() + (kTileLabelOffset * 0.5f),
        tile.w - (theme::metrics::kContentPadding * 1.2f), kTileLabelOffset};
    const Rect value_area{label_area.left(), tile.top() + kTileValueOffset, label_area.w,
        tile.h - kTileValueOffset - 6.0f};
    draw::text_clipped(draw_list, label_area, readout.label.c_str(),
        util::with_alpha(theme::color::kTextMuted, alpha), Align::Left,
        theme::metrics::kFooterFontSize);
    draw::text_clipped(draw_list, value_area, readout.value.c_str(),
        util::with_alpha(theme::color::kText, alpha), Align::Left);
}

void draw_tiles(ImDrawList* draw_list, const Rect& area, float alpha) noexcept {
    // Density is a single animated value: 0 lays the readouts out as a 2-column grid, 1 as
    // full-width list rows. One AnimState drives both the tile size and the column count, so the
    // two can never disagree mid-animation.
    State& s = state();
    const float density = util::clamp01(s.animation.value(s.density));
    const float gap = theme::metrics::kTileGap;
    const float columns_span = 2.0f;
    const float grid_width = (area.w - gap) * 0.5f;
    const float tile_width = util::lerp(grid_width, area.w, density);
    const float tile_height = util::lerp(theme::metrics::kCardHeight, theme::metrics::kRowHeight,
        density);

    float x = area.left();
    float y = area.top();
    float column = 0.0f;

    for (const Readout& readout : s.readouts) {
        draw_tile(draw_list, Rect{x, y, tile_width, tile_height}, readout, alpha);

        column += 1.0f;
        if (density >= 0.999f) {
            y += tile_height + gap;
            continue;
        }
        if (column >= columns_span) {
            column = 0.0f;
            x = area.left();
            y += tile_height + gap;
        } else {
            x += tile_width + gap;
        }

        if (y + tile_height > area.bottom()) {
            break;
        }
    }
}

void draw_theme_swatches(ImDrawList* draw_list, const Rect& area, float alpha) noexcept {
    struct Swatch {
        const char* name;
        Rgba color;
    };
    const Swatch swatches[] = {
        {"backdrop", theme::color::kWindowBackdrop},
        {"border", theme::color::kWindowBorder},
        {"chrome", theme::color::kChromeBar},
        {"sidebar", theme::color::kSidebar},
        {"card", theme::color::kCard},
        {"card hover", theme::color::kCardHover},
        {"text", theme::color::kText},
        {"muted", theme::color::kTextMuted},
        {"accent", theme::color::kAccent},
        {"accent alt", theme::color::kAccentAlt},
        {"close", theme::color::kTrafficRed},
        {"minimize", theme::color::kTrafficYellow},
        {"zoom", theme::color::kTrafficGreen},
    };

    const float width = (area.w - (theme::metrics::kTileGap * 2.0f)) / 3.0f;
    const float height = 58.0f;
    float x = area.left();
    float y = area.top();
    float column = 0.0f;

    for (const Swatch& swatch : swatches) {
        const Rect tile{x, y, width, height};
        draw::rounded_rect(draw_list, tile, util::with_alpha(swatch.color, alpha),
            theme::metrics::kFrameRounding);
        draw::border_stroke(draw_list, tile, util::with_alpha(theme::color::kCardBorder, alpha),
            theme::metrics::kFrameRounding);

        FixedString<16> hex;
        hex.format("#%02X%02X%02X", static_cast<unsigned int>(swatch.color.r * 255.0f + 0.5f),
            static_cast<unsigned int>(swatch.color.g * 255.0f + 0.5f),
            static_cast<unsigned int>(swatch.color.b * 255.0f + 0.5f));

        draw::text(draw_list, tile.left() + 8.0f, tile.bottom() - 20.0f, hex.c_str(),
            util::with_alpha(theme::color::kTextMuted, alpha), theme::metrics::kFooterFontSize);
        draw::text(draw_list, tile.left() + 8.0f, tile.bottom() - 36.0f, swatch.name,
            util::with_alpha(theme::color::kText, alpha), theme::metrics::kFooterFontSize);

        column += 1.0f;
        if (column >= 3.0f) {
            column = 0.0f;
            x = area.left();
            y += height + theme::metrics::kTileGap;
        } else {
            x += width + theme::metrics::kTileGap;
        }
        if (y + height > area.bottom()) {
            break;
        }
    }
}

void draw_empty_state(ImDrawList* draw_list, const Rect& area, float alpha) noexcept {
    const State& s = state();
    FixedString<48> planned;
    planned.format("Planned in this category: %zu module(s)",
        category_catalog_size(s.selected));
    // An empty list has two very different causes and the user should not have to guess which:
    // nothing is registered yet, or the query matches nothing.
    const char* headline = s.search.empty()
        ? "No modules registered in this category yet."
        : "No modules match the search.";
    draw::text_in(draw_list, area, headline,
        util::with_alpha(theme::color::kTextMuted, alpha), Align::Center);
    draw::text_in(draw_list, area.offset(0.0f, 22.0f), planned.c_str(),
        util::with_alpha(theme::color::kTextDim, alpha), Align::Center,
        theme::metrics::kFooterFontSize);
}

void draw_content(ImDrawList* draw_list, const Layout& layout, float collapse) noexcept {
    State& s = state();
    if (collapse >= 0.999f || layout.content.empty()) {
        return;
    }
    const float alpha = 1.0f - collapse;
    draw::rounded_rect(draw_list, layout.content,
        util::with_alpha(theme::color::kContent, alpha), theme::metrics::kWindowRounding,
        ImDrawFlags_RoundCornersBottomRight);

    const Rect body = content_body(layout.content);
    draw::text(draw_list, body.left(), body.top(), section_label(s.selected),
        util::with_alpha(theme::color::kText, alpha), theme::metrics::kHeaderFontSize);

    const Rgba accent =
        util::with_alpha(theme::category_accent(s.selected), alpha);
    const float title_bottom = body.top() + theme::metrics::kHeaderFontSize + 6.0f;
    draw::line(draw_list, body.left(), title_bottom, body.left() + 42.0f, title_bottom, accent, 2.0f);

    // The sub-badge: the live counter line for a module category (formatted when the list
    // changed, not per frame), the static section description everywhere else.
    const char* subtitle = is_module_category(s.selected)
        ? s.module_counts.c_str()
        : section_subtitle(s.selected);
    draw::text_clipped(draw_list,
        Rect{body.left(), title_bottom + 6.0f, body.w - kSearchWidth - 8.0f, 18.0f}, subtitle,
        util::with_alpha(theme::color::kTextMuted, alpha), Align::Left,
        theme::metrics::kFooterFontSize);

    // The search field belongs to every page; it is drawn by its own component so the caret and
    // the magnifier are the same everywhere.
    s.search.render(draw_list, search_field_rect(layout.content));

    const Rect content_area = card_pane(layout.content);
    if (content_area.empty()) {
        return;
    }

    if (s.selected == Section::Diagnostics) {
        draw_tiles(draw_list, content_area, alpha);
    } else if (s.selected == Section::ThemePage) {
        draw_theme_swatches(draw_list, content_area, alpha);
    } else if (is_module_category(s.selected)) {
        // Live count from the registry, not cached state: switching categories must not show the
        // previous category's cards or a stale empty message.
        const bool has_modules =
            modules::manager().category_total(modules::category_from_section(s.selected)) > 0;
        if (has_modules) {
            draw_module_cards(draw_list);
            // The rows come after every card, so the open drawer's shell is already beneath them.
            draw_settings_rows(draw_list, alpha);
        } else {
            draw_empty_state(draw_list, content_area, alpha);
        }
    } else {
        draw_empty_state(draw_list, content_area, alpha);
    }
}

// ── Frame pieces ──────────────────────────────────────────────────────────────────

void tick_animation(double now) noexcept {
    State& s = state();
    const double delta = s.have_frame_time ? (now - s.last_frame_seconds) : 0.0;
    s.last_frame_seconds = now;
    s.have_frame_time = true;

    float seconds = static_cast<float>(delta);
    if (!(seconds > 0.0f)) {
        seconds = 0.0f;
    }
    if (seconds > static_cast<float>(kMaxFrameDeltaSeconds)) {
        seconds = static_cast<float>(kMaxFrameDeltaSeconds);
    }

    s.animation.set_target(s.open_spring, s.visible ? 1.0f : 0.0f);
    s.animation.set_target(s.collapse, s.collapsed ? 1.0f : 0.0f);
    s.animation.set_target(s.density, s.grid_layout ? 0.0f : 1.0f);
    s.animation.tick(seconds);

    // The toast pool advances on the same clock, after the controller so it reads post-tick
    // reveal values: one clock, one tick (§7.4).
    s.notifications.animate(seconds);
    sync_overlay_request();
    // The in-world layer draws here: inside the live frame, after the animation tick, and it
    // yields to the ClickGUI itself so a crosshair can never land on top of the module cards.
    render_inworld_overlay();
}

// The toasts are drawn outside the ClickGUI window on purpose: they must render on a frame where
// the chrome is hidden, because a toast outlives the GUI being closed.
void render_notifications_layer() noexcept {
    const ImGuiIO& io = ImGui::GetIO();
    const Rect screen{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y};
    State& s = state();
    s.notifications.set_screen(screen);
    s.notifications.render(ImGui::GetForegroundDrawList(), screen);
}

} // namespace detail
} // namespace woke::ui
