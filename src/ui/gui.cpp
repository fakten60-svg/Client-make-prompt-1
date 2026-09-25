#include "ui/gui.h"

#include <windows.h>

#include <array>
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
#include "core/version.h"
#include "hooks/game_thread.h"
#include "hooks/hook_manager.h"
#include "hooks/swap_hook.h"
#include "hooks/wndproc_hook.h"
#include "modules/category.h"
#include "modules/module_manager.h"
#include "modules/movement/velocity_display.h"
#include "modules/visual/custom_crosshair.h"
#include "modules/visual/hud_module.h"
#include "modules/visual/trajectories.h"
#include "ui/animation/animation_controller.h"
#include "ui/animation/easing.h"
#include "ui/components/keybind_badge.h"
#include "ui/components/module_card.h"
#include "ui/components/search_bar.h"
#include "ui/components/sidebar.h"
#include "ui/components/traffic_lights.h"
#include "ui/hud.h"
#include "ui/notifications.h"
#include "ui/theme.h"
#include "utils/math_utils.h"
#include "utils/render_utils.h"
#include "utils/string_buffer.h"

#if WOKE_HAVE_JNI
#include "jni/game_instance.h"
#include "jni/reflection_cache.h"
#endif

// ImGui's Win32 backend deliberately leaves its message handler commented out in its header (it
// does not want to pull <windows.h> into the helper), and expects the application to forward
// declare it - this is the declaration the upstream Win32 example uses. It must stay at global
// scope: the definition in imgui_impl_win32.cpp is not namespaced.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace woke::ui {
namespace {

using animation::AnimationController;
using animation::StateHandle;
using components::Input;
using components::ModuleCard;
using components::SearchBar;
using components::Sidebar;
using components::TrafficLights;
using draw::Align;
using draw::Rect;
using util::FixedString;
using util::Rgba;

namespace modules = woke::modules;

// The Misc/ClickGUI bind (§8: default RSHIFT).
constexpr int kDefaultToggleKey = VK_RSHIFT;

// Readouts are recomputed four times a second. The numbers on screen change at human speed, and
// formatting ten labels per frame would be ten snprintf calls of pure waste inside the render
// thread - the overlay's own budget is 0.2 ms (§7.4 gate).
constexpr double kReadoutIntervalSeconds = 0.25;

// §6.4 sets the whole-pipeline budget at 0.5 ms; this is the chrome's own share of it.
constexpr float kChromeBudgetMs = 0.2f;
constexpr double kChromeWarningCooldownSeconds = 10.0;

constexpr std::uint64_t kHeartbeatFrames = 600;
constexpr double kMaxFrameDeltaSeconds = 0.25;
constexpr std::size_t kReadoutCount = 10;

constexpr float kDensityButtonWidth = 62.0f;
constexpr float kDensityButtonHeight = 22.0f;
constexpr float kTileLabelOffset = 12.0f;
constexpr float kTileValueOffset = 32.0f;

// Content-pane metrics the composition owns: the search field in the header row, and the inset of
// a settings row inside an expanded card's drawer. Everything card-shaped (body, drawer, chevron,
// chip, pill) is sized by ModuleCard itself, so there is one definition of a card's geometry.
constexpr float kSearchWidth = 220.0f;
constexpr float kSearchHeight = 26.0f;
constexpr float kSettingRowInset = 12.0f;
constexpr float kSettingValueColumn = 96.0f;

// Vertical distance from the content pane's padding box to where the module cards start: the
// heading, its subtitle and one spacer row (matches draw_content's own layout math).
constexpr float kTileHeaderOffset = 32.0f;

struct Readout {
    FixedString<24> label;
    FixedString<44> value;
};

struct State {
    bool initialized = false;
    bool visible = false;
    bool collapsed = false;
    bool dragging = false;
    bool window_positioned = false;
    bool renderer_ready = false;
    bool renderer_failed = false;
    bool grid_layout = true;
    bool suppression_logged = true; // true until a close is reported, so boot is quiet
    int toggle_key = kDefaultToggleKey;

    HWND window = nullptr;
    ImGuiContext* context = nullptr;

    events::Subscription key_subscription{};
    events::Subscription focus_subscription{};

    AnimationController animation{};
    animation::SpringHandle open_spring{};
    StateHandle collapse{};
    StateHandle density{};
    TrafficLights lights{};

    // The widget library (roadmap step 6). One Sidebar, one SearchBar, one toast pool and one
    // ModuleCard per registry row - all constructed once at boot, so no frame ever allocates a
    // widget or an animation slot (§6.3). Cards are indexed by *registry* index: a filter change
    // or a rebind therefore cannot move a card's animation slots onto a different module.
    Sidebar sidebar{};
    SearchBar search{};
    Notifications notifications{};
    std::array<ModuleCard, modules::ModuleManager::kMaxModules> cards{};

    // The filtered module list for this frame, in registry order, with the registry index behind
    // each entry. Rebuilt only when the query or the registry changes - never per frame (§7.1).
    std::array<modules::BaseModule*, modules::ModuleManager::kMaxModules> visible_modules{};
    std::array<std::size_t, modules::ModuleManager::kMaxModules> visible_indices{};
    std::size_t visible_count = 0;
    std::size_t cached_module_count = 0;
    bool list_dirty = true;
    // The header's "n/m shown | k enabled" line, formatted when the list changes (§7.1), not per
    // frame.
    FixedString<48> module_counts{};

    // The one open settings drawer (by module, so it survives a filter change) and the keybind
    // capture in progress (by registry index, so it survives one too). nullptr / -1 = none.
    modules::BaseModule* expanded_module = nullptr;
    int capture_index = -1;
    settings::Setting* slider_drag = nullptr;

    Section selected = Section::Diagnostics;

    float drag_offset_x = 0.0f;
    float drag_offset_y = 0.0f;
    ImVec2 window_position = ImVec2(0.0f, 0.0f);

    Input input{};

    LARGE_INTEGER frequency{};
    double last_frame_seconds = 0.0;
    bool have_frame_time = false;

    std::size_t frames_rendered = 0;
    std::size_t suppressed_frames = 0;
    float last_chrome_ms = 0.0f;
    float average_chrome_ms = 0.0f;
    float worst_chrome_ms = 0.0f;
    double last_chrome_warning_seconds = -1.0;

    std::array<Readout, kReadoutCount> readouts{};
    double readout_refresh_at = 0.0;

    // Interaction targets computed once per frame in screen space, so input handling and drawing
    // cannot disagree about where a button is.
    Rect header{};
    Rect density_button{};
    // Cached module card rectangles for the selected category, refreshed when the page or the
    // module layout changes; input handling reads these instead of recomputing layout.
    std::array<Rect, modules::ModuleManager::kMaxModules> card_rects{};
    std::array<std::size_t, modules::ModuleManager::kMaxModules> card_registry{};
    std::size_t card_count = 0;
};

State g_state{};

// ── Timing ────────────────────────────────────────────────────────────────────────

double now_seconds() noexcept {
    if (g_state.frequency.QuadPart <= 0) {
        return 0.0;
    }
    LARGE_INTEGER counter{};
    (void)::QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(g_state.frequency.QuadPart);
}

float milliseconds_between(const LARGE_INTEGER& start, const LARGE_INTEGER& end) noexcept {
    if (g_state.frequency.QuadPart <= 0) {
        return 0.0f;
    }
    const double ticks = static_cast<double>(end.QuadPart - start.QuadPart);
    return static_cast<float>((ticks * 1000.0) / static_cast<double>(g_state.frequency.QuadPart));
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

// ── Lazy renderer initialisation ──────────────────────────────────────────────────

bool ensure_renderer() noexcept {
    if (g_state.renderer_ready) {
        return true;
    }
    if (g_state.renderer_failed) {
        return false;
    }
    if (g_state.context == nullptr) {
        return false;
    }
    if (g_state.window == nullptr) {
        // Normal early state: the DLL can be injected before the WndProc hook has found the game
        // window. Nothing is logged per frame - the boot log already says input is unavailable.
        return false;
    }

    ImGui::SetCurrentContext(g_state.context);

    // The Win32 backend in its OpenGL flavour (monitor DPI + cursor handling for a GL window), and
    // then the GL3 backend. This runs inside the swap detour, where the game's GL context is
    // current by definition - the one place a GL initialisation is guaranteed to be valid.
    if (!ImGui_ImplWin32_InitForOpenGL(g_state.window)) {
        g_state.renderer_failed = true;
        WOKE_LOG_ERROR("gui-renderer: the ImGui Win32 backend could not attach to window %p",
            static_cast<void*>(g_state.window));
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(nullptr)) {
        ImGui_ImplWin32_Shutdown();
        g_state.renderer_failed = true;
        WOKE_LOG_ERROR("gui-renderer: the ImGui OpenGL3 backend could not initialise");
        return false;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // The game owns the OS cursor while it has raw input, so the overlay draws its own. Without
    // this the ClickGUI would be unusable in a captured window.
    io.MouseDrawCursor = true;

    g_state.renderer_ready = true;
    WOKE_LOG_INFO("gui-renderer: ImGui %s ready on window %p (win32 + opengl3)", IMGUI_VERSION,
        static_cast<void*>(g_state.window));
    return true;
}

// ── Readouts ──────────────────────────────────────────────────────────────────────

template <typename... Args>
void set_readout(std::size_t index, const char* label, const char* format, Args... args) noexcept {
    if (index >= kReadoutCount) {
        return;
    }
    g_state.readouts[index].label.assign(label);
    g_state.readouts[index].value.format(format, args...);
}

const char* overlay_state_text() noexcept {
    if (g_state.renderer_failed) {
        return "backend unavailable";
    }
    if (!g_state.visible) {
        return "hidden - zero draw calls";
    }
    if (!g_state.renderer_ready) {
        return "waiting for the game window";
    }
    return "rendering";
}

void refresh_readouts(double now) noexcept {
    if (now < g_state.readout_refresh_at) {
        return;
    }
    g_state.readout_refresh_at = now + kReadoutIntervalSeconds;

    const hooks::game_thread::Stats pipeline = hooks::game_thread::stats();

    set_readout(0, "Frame rate", "%.1f fps", static_cast<double>(pipeline.frames_per_second));
    set_readout(1, "Frames / ticks", "%llu / %llu",
        static_cast<unsigned long long>(pipeline.frames),
        static_cast<unsigned long long>(pipeline.ticks));
    set_readout(2, "Pipeline cost", "avg %.3f ms",
        static_cast<double>(pipeline.average_pipeline_ms));
    set_readout(3, "Chrome cost", "avg %.3f ms", static_cast<double>(g_state.average_chrome_ms));
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

struct Layout {
    Rect window{};
    Rect header{};
    Rect body{};
    Rect rail{};
    Rect content{};
};

Layout compute_layout(float open_amount, float collapse) noexcept {
    const ImGuiIO& io = ImGui::GetIO();
    const float scale =
        util::lerp(theme::metrics::kOpenScaleFrom, 1.0f, animation::ease_out_expo(open_amount));

    const float width = util::lerp(theme::metrics::kWindowWidth, theme::metrics::kCollapsedWidth,
                            collapse)
        * scale;
    const float height = util::lerp(theme::metrics::kWindowHeight,
                             theme::metrics::kCollapsedHeight, collapse)
        * scale;

    if (!g_state.window_positioned) {
        g_state.window_position = ImVec2((io.DisplaySize.x - width) * 0.5f,
            (io.DisplaySize.y - height) * 0.5f);
        g_state.window_positioned = true;
    }

    // Keep the window reachable even if the display was resized under it.
    const float max_x = io.DisplaySize.x > width ? io.DisplaySize.x - width : 0.0f;
    const float max_y = io.DisplaySize.y > height ? io.DisplaySize.y - height : 0.0f;
    g_state.window_position.x = util::clamp(g_state.window_position.x, 0.0f, max_x);
    g_state.window_position.y = util::clamp(g_state.window_position.y, 0.0f, max_y);

    Layout layout;
    layout.window = Rect{g_state.window_position.x, g_state.window_position.y, width, height};
    layout.header = layout.window.slice_top(util::lerp(theme::metrics::kHeaderHeight,
        theme::metrics::kCollapsedHeight, collapse));
    layout.body = layout.window.without_top(layout.header.h);
    const float sidebar = util::lerp(theme::metrics::kSidebarWidth, 0.0f, collapse);
    layout.rail = layout.body.slice_left(sidebar);
    layout.content = layout.body.without_left(sidebar);
    return layout;
}

// ── Input snapshot ────────────────────────────────────────────────────────────────

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
    const Input& input = g_state.input;

    if (g_state.dragging) {
        if (input.is_down(0)) {
            g_state.window_position.x = input.mouse_x - g_state.drag_offset_x;
            g_state.window_position.y = input.mouse_y - g_state.drag_offset_y;
            g_state.window_positioned = true;
        } else {
            g_state.dragging = false;
        }
        return;
    }

    // A drag starts on the chrome bar, but not on a traffic light or the density button: those
    // consume their own press, and the header is otherwise the window's grab handle.
    if (!input.is_pressed(0) || g_state.collapsed) {
        return;
    }
    if (!layout.header.contains(input.mouse_x, input.mouse_y)) {
        return;
    }
    if (g_state.density_button.contains(input.mouse_x, input.mouse_y)) {
        return;
    }

    const Rect lights = Rect{layout.header.left() + theme::metrics::kTrafficLightInset,
        layout.header.top(), theme::metrics::kTrafficLightGap * 3.0f, layout.header.h};
    for (std::size_t index = 0; index < TrafficLights::kCount; ++index) {
        if (g_state.lights.button_rect(lights, index).inset(-4.0f).contains(input.mouse_x,
                input.mouse_y)) {
            return;
        }
    }

    g_state.dragging = true;
    g_state.drag_offset_x = input.mouse_x - g_state.window_position.x;
    g_state.drag_offset_y = input.mouse_y - g_state.window_position.y;
}

// Pushes the registry's counters and the frame rate into the sidebar. Two int reads per category
// by design (§4.4): the manager precomputes the buckets, so this is six lookups, not a scan.
void sync_sidebar() noexcept {
    const modules::ModuleManager& registry = modules::manager();
    for (std::size_t index = 0; index < kModuleCategoryCount; ++index) {
        const modules::Category category =
            modules::category_from_section(static_cast<Section>(index));
        g_state.sidebar.set_counts(index, registry.category_enabled(category),
            registry.category_total(category));
    }

    const hooks::game_thread::Stats pipeline = hooks::game_thread::stats();
    FixedString<64> footer;
    footer.format("%s %s | %.0f fps", version::kClientName, version::kVersion,
        static_cast<double>(pipeline.frames_per_second));
    g_state.sidebar.set_footer(footer.c_str());
}

// The content pane's padding box: where the heading, the subtitle and the search field live.
[[nodiscard]] Rect content_body(const Rect& content) noexcept {
    return content.inset(theme::metrics::kContentPadding);
}

// The search field, right-aligned on the heading row. Input handling and drawing both call this,
// so the hit target and the field cannot drift apart.
[[nodiscard]] Rect search_field_rect(const Rect& content) noexcept {
    const Rect body = content_body(content);
    return Rect{body.right() - kSearchWidth, body.top() - 2.0f, kSearchWidth, kSearchHeight};
}

// The rectangle the module cards occupy: the body below the heading, its accent underline and its
// subtitle line. One definition for input and drawing (the step-4 note promised exactly this).
[[nodiscard]] Rect card_pane(const Rect& content) noexcept {
    const Rect body = content_body(content);
    const float header_drop = kTileHeaderOffset + theme::metrics::kHeaderFontSize + 6.0f;
    return Rect{body.left(), body.top() + header_drop, body.w,
        body.h > header_drop ? body.h - header_drop : 0.0f};
}

void handle_lights(const Layout& layout) noexcept {
    const Rect lights = Rect{layout.header.left() + theme::metrics::kTrafficLightInset,
        layout.header.top(), theme::metrics::kTrafficLightGap * 3.0f, layout.header.h};
    g_state.lights.set_area(lights);
    (void)g_state.lights.handle_input(g_state.input);

    switch (g_state.lights.take_action()) {
    case TrafficLights::Action::Close:
        // Red hides the GUI; it never unloads the client (§3.6).
        set_visible(false);
        break;
    case TrafficLights::Action::Minimize:
        // Yellow collapses to a pill that stays clickable, so the window can be brought back
        // without a keybind.
        g_state.collapsed = !g_state.collapsed;
        g_state.window_positioned = true;
        break;
    case TrafficLights::Action::Zoom:
        // Green toggles the content density: grid <-> list, the same axis the module grid uses.
        g_state.grid_layout = !g_state.grid_layout;
        g_state.animation.set_target(g_state.density, g_state.grid_layout ? 0.0f : 1.0f);
        break;
    case TrafficLights::Action::None:
        break;
    }
}

void handle_density_button() noexcept {
    if (!g_state.density_button.contains(g_state.input.mouse_x, g_state.input.mouse_y)) {
        return;
    }
    if (g_state.input.is_pressed(0)) {
        g_state.grid_layout = !g_state.grid_layout;
        g_state.animation.set_target(g_state.density, g_state.grid_layout ? 0.0f : 1.0f);
    }
}

// ── Step 6: the module list, its cards and the settings drawers ────────────────

// The in-world overlay's entry points, defined with the rest of that layer further down. They are
// declared here because sync_overlay_request runs every frame and the frame scheduler's flag has
// to include them (§7.6, §7.8).
bool inworld_overlay_wanted() noexcept;
void render_inworld_overlay() noexcept;
void sync_clickgui_module() noexcept;

// The overlay must also stay awake for something that is not the chrome: a toast outlives the
// ClickGUI being hidden, and the frame scheduler's predicate reads this flag.
void sync_overlay_request() noexcept {
    // The ClickGUI module mirrors visibility whichever way it changed: a card click, a keybind, or
    // the overlay's own red traffic light.
    sync_clickgui_module();
    hooks::game_thread::set_overlay_requested(g_state.visible || g_state.notifications.needs_render()
        || inworld_overlay_wanted());
}

void push_toast(const char* title, const char* message, ToastIcon icon) noexcept {
    Toast toast;
    toast.title = title;
    toast.message = message;
    toast.icon = icon;
    if (!g_state.notifications.push(toast)) {
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

// Rebuilds the filtered module list for the selected category, and the header's counter line with
// it. Runs on a query change or a registry change - never on a timer (§7.1).
void recompute_visible_modules() noexcept {
    const modules::ModuleManager& registry = modules::manager();
    const modules::Category category = modules::category_from_section(g_state.selected);
    std::size_t count = 0;
    for (std::size_t index = 0; index < registry.count(); ++index) {
        modules::BaseModule* module = registry.at(index);
        if (module == nullptr || module->category() != category) {
            continue;
        }
        // A query matches the name or the description: "zoom" should find Zoom, and "sprint"
        // should also find the module that is *described* as a sprint helper.
        if (!g_state.search.empty() && !g_state.search.matches(module->name())
            && !g_state.search.matches(module->description())) {
            continue;
        }
        g_state.visible_modules[count] = module;
        g_state.visible_indices[count] = index;
        ++count;
    }
    g_state.visible_count = count;
    g_state.list_dirty = false;

    // Formatted here rather than per frame: the numbers move only when the list does (§7.1).
    g_state.module_counts.format("%zu/%zu shown  |  %zu enabled", g_state.visible_count,
        registry.category_total(category), registry.category_enabled(category));
}

// Card rectangles for the visible modules, laid out exactly as draw_module_cards places them, so
// a click can never land on a card the frame has not drawn. Grid density puts two half-width
// tiles per row; list density - and an *expanded* card - takes the full width, because a drawer
// needs the room and a half-width drawer would clip its own rows.
void layout_module_cards(const Rect& pane) noexcept {
    const float density = util::clamp01(g_state.animation.value(g_state.density));
    const float gap = theme::metrics::kTileGap;
    const float half_width = (pane.w - gap) * 0.5f;
    const float body = ModuleCard::body_height(density);
    const bool list_mode = density >= 0.999f;

    float x = pane.left();
    float y = pane.top();
    bool right_column = false;
    float row_height = 0.0f;
    std::size_t placed = 0;

    for (std::size_t index = 0; index < g_state.visible_count; ++index) {
        modules::BaseModule* module = g_state.visible_modules[index];
        const bool expanded = (module == g_state.expanded_module);
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

        g_state.card_rects[placed] = Rect{x, y, width, height};
        g_state.card_registry[placed] = g_state.visible_indices[index];
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
    g_state.card_count = placed;
}

// The card currently carrying an open drawer, by layout slot. kMaxModules means "none".
[[nodiscard]] std::size_t expanded_slot() noexcept {
    if (g_state.expanded_module == nullptr) {
        return modules::ModuleManager::kMaxModules;
    }
    for (std::size_t slot = 0; slot < g_state.card_count; ++slot) {
        if (modules::manager().at(g_state.card_registry[slot]) == g_state.expanded_module) {
            return slot;
        }
    }
    return modules::ModuleManager::kMaxModules;
}

// Where a settings row sits inside a drawer. The *card* owns the drawer shell; its contents are
// the GUI's, because the GUI is the layer that knows about settings (ModuleCard stays independent
// of the module/setting model, which is what lets the host tests drive it standalone).
[[nodiscard]] Rect setting_row_rect(const Rect& drawer, std::size_t row) noexcept {
    return Rect{drawer.left() + kSettingRowInset,
        drawer.top() + ModuleCard::kDrawerPadding
            + (static_cast<float>(row) * ModuleCard::kDrawerRowHeight),
        drawer.w - (kSettingRowInset * 2.0f), ModuleCard::kDrawerRowHeight};
}

// The open drawer's rectangle in this frame's layout; empty when nothing is expanded or the card
// fell outside the visible pane.
[[nodiscard]] Rect expanded_drawer_rect() noexcept {
    const std::size_t slot = expanded_slot();
    if (slot >= g_state.card_count) {
        return Rect{};
    }
    const std::size_t registry_index = g_state.card_registry[slot];
    const modules::BaseModule* module = modules::manager().at(registry_index);
    if (module == nullptr) {
        return Rect{};
    }
    return ModuleCard::drawer_area_for(g_state.card_rects[slot], module->setting_count());
}

void handle_module_cards(const Rect& pane, float alpha) noexcept {
    layout_module_cards(pane);

    const Input& input = g_state.input;
    for (std::size_t slot = 0; slot < g_state.card_count; ++slot) {
        const std::size_t registry_index = g_state.card_registry[slot];
        modules::BaseModule* module = modules::manager().at(registry_index);
        if (module == nullptr) {
            continue;
        }

        ModuleCard& card = g_state.cards[registry_index];
        card.set_area(g_state.card_rects[slot]);
        card.set_alpha(alpha);
        card.set_content(module->name(), module->description(), module->bind());
        card.set_enabled(module->enabled());
        card.set_expanded(module == g_state.expanded_module);
        card.set_drawer_rows(module->setting_count());
        // Exactly one card can be the capture target, because there is one capture index.
        card.set_capture_armed(g_state.capture_index == static_cast<int>(registry_index));

        // All mutation happens in this input phase (§7.3: render never mutates); the draw phase
        // only reads the resulting state.
        const ModuleCard::Result result = card.interact(input);
        if (result.bind_clicked) {
            const bool arming = g_state.capture_index != static_cast<int>(registry_index);
            g_state.capture_index = arming ? static_cast<int>(registry_index) : -1;
            if (arming) {
                WOKE_LOG_DEBUG("keybind: capturing a new bind for '%s'", module->name());
            }
            continue; // a chip click is never also a toggle
        }
        if (result.expand_toggled) {
            // One drawer at a time: a second open drawer would push the card being read off the
            // pane, and this layout has no scrolling yet.
            g_state.expanded_module = (g_state.expanded_module == module) ? nullptr : module;
            continue;
        }
        if (result.toggled && module->set_enabled(!module->enabled())) {
            modules::manager().notify_changed();
            (void)save_config();
            card.set_enabled(module->enabled()); // the pill must not lag the click by a frame
            g_state.list_dirty = true;            // the sidebar's counter badges changed
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
            g_state.capture_index = static_cast<int>(index);
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
    modules::BaseModule* module = g_state.expanded_module;
    if (module == nullptr) {
        g_state.slider_drag = nullptr;
        return;
    }
    const Rect drawer = expanded_drawer_rect();
    if (drawer.empty()) {
        return;
    }

    const Input& input = g_state.input;
    for (std::size_t row = 0; row < module->setting_count(); ++row) {
        settings::Setting* setting = module->settings()[row];
        if (setting == nullptr) {
            continue;
        }
        const Rect rect = setting_row_rect(drawer, row);
        const bool hovered = rect.contains(input.mouse_x, input.mouse_y);

        if (setting->kind() == settings::Kind::Slider) {
            if (hovered && input.is_pressed(0)) {
                g_state.slider_drag = setting;
            }
            if (g_state.slider_drag == setting) {
                if (input.is_down(0)) {
                    const float t = util::inverse_lerp(rect.left(), rect.right(), input.mouse_x);
                    setting->set_float(
                        util::lerp(setting->float_minimum(), setting->float_maximum(), t));
                } else {
                    g_state.slider_drag = nullptr;
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
    const std::size_t slot = expanded_slot();
    const modules::BaseModule* module = g_state.expanded_module;
    if (module == nullptr || slot >= g_state.card_count || module->setting_count() == 0) {
        return;
    }
    // The rows fade with the drawer's own reveal, so they can never be visible through a closed
    // drawer shell.
    const float open = g_state.cards[g_state.card_registry[slot]].open_amount();
    if (open <= 0.01f) {
        return;
    }
    const Rect drawer =
        ModuleCard::drawer_area_for(g_state.card_rects[slot], module->setting_count());
    const float row_alpha = alpha * open;
    const Input& input = g_state.input;

    for (std::size_t row = 0; row < module->setting_count(); ++row) {
        const settings::Setting* setting = module->settings()[row];
        if (setting == nullptr) {
            continue;
        }
        const Rect rect = setting_row_rect(drawer, row);
        const bool active = rect.contains(input.mouse_x, input.mouse_y)
            || g_state.slider_drag == setting;

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
    for (std::size_t slot = 0; slot < g_state.card_count; ++slot) {
        const std::size_t registry_index = g_state.card_registry[slot];
        const Rect& card = g_state.card_rects[slot];
        if (card.empty()) {
            continue;
        }
        // No state is mutated here: the rectangles, the alpha and every target were set in this
        // frame's input phase, and the card renders itself from them (§7.3).
        g_state.cards[registry_index].render(draw_list, card);
    }
}

// ── Drawing ───────────────────────────────────────────────────────────────────────

FixedString<64> compose_title() noexcept {
    FixedString<64> title;
    title.assign(version::kClientName);
    if (!g_state.collapsed) {
        title.append("  ·  ");
        title.append(section_label(g_state.selected));
    }
    return title;
}

void draw_chrome_bar(ImDrawList* draw_list, const Layout& layout, float collapse) noexcept {
    const Rgba chrome = util::with_alpha(theme::color::kChromeBar, 1.0f - (0.6f * collapse));
    draw::rounded_rect(draw_list, layout.header, chrome, theme::metrics::kWindowRounding,
        ImDrawFlags_RoundCornersTop);

    g_state.lights.render(draw_list, Rect{layout.header.left() + theme::metrics::kTrafficLightInset,
                                      layout.header.top(), theme::metrics::kTrafficLightGap * 3.0f,
                                      layout.header.h});

    const FixedString<64> title = compose_title();
    draw::text_in(draw_list, layout.header, title.c_str(), theme::color::kText, Align::Center,
        theme::metrics::kHeaderFontSize);

    if (collapse < 0.5f) {
        const char* density = g_state.grid_layout ? "Grid" : "List";
        draw::rounded_rect(draw_list, g_state.density_button, theme::color::kCard,
            theme::metrics::kFrameRounding);
        draw::border_stroke(draw_list, g_state.density_button, theme::color::kCardBorder,
            theme::metrics::kFrameRounding);
        draw::text_in(draw_list, g_state.density_button, density, theme::color::kTextMuted,
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
    const float density = util::clamp01(g_state.animation.value(g_state.density));
    const float gap = theme::metrics::kTileGap;
    const float columns_span = 2.0f;
    const float grid_width = (area.w - gap) * 0.5f;
    const float tile_width = util::lerp(grid_width, area.w, density);
    const float tile_height = util::lerp(theme::metrics::kCardHeight, theme::metrics::kRowHeight,
        density);

    float x = area.left();
    float y = area.top();
    float column = 0.0f;

    for (const Readout& readout : g_state.readouts) {
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
    FixedString<48> planned;
    planned.format("Planned in this category: %zu module(s)",
        category_catalog_size(g_state.selected));
    // An empty list has two very different causes and the user should not have to guess which:
    // nothing is registered yet, or the query matches nothing.
    const char* headline = g_state.search.empty()
        ? "No modules registered in this category yet."
        : "No modules match the search.";
    draw::text_in(draw_list, area, headline,
        util::with_alpha(theme::color::kTextMuted, alpha), Align::Center);
    draw::text_in(draw_list, area.offset(0.0f, 22.0f), planned.c_str(),
        util::with_alpha(theme::color::kTextDim, alpha), Align::Center,
        theme::metrics::kFooterFontSize);
}

void draw_content(ImDrawList* draw_list, const Layout& layout, float collapse) noexcept {
    if (collapse >= 0.999f || layout.content.empty()) {
        return;
    }
    const float alpha = 1.0f - collapse;
    draw::rounded_rect(draw_list, layout.content,
        util::with_alpha(theme::color::kContent, alpha), theme::metrics::kWindowRounding,
        ImDrawFlags_RoundCornersBottomRight);

    const Rect body = content_body(layout.content);
    draw::text(draw_list, body.left(), body.top(), section_label(g_state.selected),
        util::with_alpha(theme::color::kText, alpha), theme::metrics::kHeaderFontSize);

    const Rgba accent =
        util::with_alpha(theme::category_accent(g_state.selected), alpha);
    const float title_bottom = body.top() + theme::metrics::kHeaderFontSize + 6.0f;
    draw::line(draw_list, body.left(), title_bottom, body.left() + 42.0f, title_bottom, accent, 2.0f);

    // The sub-badge: the live counter line for a module category (formatted when the list
    // changed, not per frame), the static section description everywhere else.
    const char* subtitle = is_module_category(g_state.selected)
        ? g_state.module_counts.c_str()
        : section_subtitle(g_state.selected);
    draw::text_clipped(draw_list,
        Rect{body.left(), title_bottom + 6.0f, body.w - kSearchWidth - 8.0f, 18.0f}, subtitle,
        util::with_alpha(theme::color::kTextMuted, alpha), Align::Left,
        theme::metrics::kFooterFontSize);

    // The search field belongs to every page; it is drawn by its own component so the caret and
    // the magnifier are the same everywhere.
    g_state.search.render(draw_list, search_field_rect(layout.content));

    const Rect content_area = card_pane(layout.content);
    if (content_area.empty()) {
        return;
    }

    if (g_state.selected == Section::Diagnostics) {
        draw_tiles(draw_list, content_area, alpha);
    } else if (g_state.selected == Section::ThemePage) {
        draw_theme_swatches(draw_list, content_area, alpha);
    } else if (is_module_category(g_state.selected)) {
        // Live count from the registry, not cached state: switching categories must not show the
        // previous category's cards or a stale empty message.
        const bool has_modules =
            modules::manager().category_total(modules::category_from_section(g_state.selected)) > 0;
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

// ── Frame ─────────────────────────────────────────────────────────────────────────

void tick_animation(double now) noexcept {
    const double delta = g_state.have_frame_time ? (now - g_state.last_frame_seconds) : 0.0;
    g_state.last_frame_seconds = now;
    g_state.have_frame_time = true;

    float seconds = static_cast<float>(delta);
    if (!(seconds > 0.0f)) {
        seconds = 0.0f;
    }
    if (seconds > static_cast<float>(kMaxFrameDeltaSeconds)) {
        seconds = static_cast<float>(kMaxFrameDeltaSeconds);
    }

    g_state.animation.set_target(g_state.open_spring, g_state.visible ? 1.0f : 0.0f);
    g_state.animation.set_target(g_state.collapse, g_state.collapsed ? 1.0f : 0.0f);
    g_state.animation.set_target(g_state.density, g_state.grid_layout ? 0.0f : 1.0f);
    g_state.animation.tick(seconds);

    // The toast pool advances on the same clock, after the controller so it reads post-tick
    // reveal values: one clock, one tick (§7.4).
    g_state.notifications.animate(seconds);
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
    g_state.notifications.set_screen(screen);
    g_state.notifications.render(ImGui::GetForegroundDrawList(), screen);
}

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
// HUD keeps the frame pipeline alive with the chrome hidden (§7.6, §7.8).
bool inworld_overlay_wanted() noexcept {
    resolve_overlay_modules();
    if (g_overlay.hud != nullptr && g_overlay.hud->draws()) {
        return true;
    }
    if (g_overlay.crosshair != nullptr && g_overlay.crosshair->enabled()) {
        return true;
    }
    if (g_overlay.velocity != nullptr && g_overlay.velocity->enabled()) {
        return true;
    }
    return g_overlay.trajectories != nullptr && g_overlay.trajectories->enabled();
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
    return g_state.notifications.needs_render();
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
