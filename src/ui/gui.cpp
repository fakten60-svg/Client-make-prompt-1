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
#include "ui/animation/animation_controller.h"
#include "ui/animation/easing.h"
#include "ui/components/traffic_lights.h"
#include "ui/theme.h"
#include "utils/math_utils.h"
#include "utils/render_utils.h"
#include "utils/string_buffer.h"

#if WOKE_HAVE_JNI
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

constexpr float kNavRowInset = 10.0f;
constexpr float kNavAccentWidth = 3.0f;
constexpr float kNavLabelInset = 14.0f;
constexpr float kSectionLabelHeight = 22.0f;
constexpr float kLogoBlockHeight = 78.0f;
constexpr float kFooterHeight = 26.0f;
constexpr float kBodyFontSize = 13.0f;
constexpr float kDensityButtonWidth = 62.0f;
constexpr float kDensityButtonHeight = 22.0f;
constexpr float kTileLabelOffset = 12.0f;
constexpr float kTileValueOffset = 32.0f;

// Module card metrics (blueprint §7.1). List rows are shorter than grid tiles by design; the
// pill toggle is the same Apple-blue switch on both.
constexpr float kCardPadding = 12.0f;
constexpr float kPillWidth = 40.0f;
constexpr float kBindBadgeWidth = 34.0f;
constexpr float kBindBadgeHeight = 18.0f;
constexpr float kBindBadgeOffset = 10.0f;
constexpr float kCardDescOffset = 20.0f;
// Vertical distance from the content pane's padding box to where the module cards start: the
// heading, its subtitle and one spacer row (matches draw_content's own layout math).
constexpr float kTileHeaderOffset = 32.0f;

// Section order in the sidebar: the six module categories from the catalogue, then the general
// pages. Derived from the enum so a new Section cannot be forgotten here.
constexpr std::size_t kNavCount = kSectionCount;

[[nodiscard]] constexpr Section nav_section(std::size_t index) noexcept {
    return static_cast<Section>(index);
}

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
    std::array<StateHandle, kNavCount> nav_hover{};
    TrafficLights lights{};

    // Per-card animation slots, acquired once at boot: index i tracks registry index i. The pill
    // toggle's t and the card's hover brightness are separate states so a shared widget cannot
    // make two cards visually desync; the registry holds at most 32 modules, so the arrays are
    // sized once and never grow (§6.3).
    std::array<animation::StateHandle, modules::ModuleManager::kMaxModules> card_hover{};
    std::array<animation::StateHandle, modules::ModuleManager::kMaxModules> card_pill{};

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

void handle_sidebar_nav(const Layout& layout, float collapse) noexcept {
    if (collapse > 0.5f || layout.rail.empty()) {
        return;
    }

    const bool clicked = g_state.input.is_pressed(0) && !g_state.dragging;
    float y = layout.rail.top() + kLogoBlockHeight + kSectionLabelHeight;

    for (std::size_t index = 0; index < kNavCount; ++index) {
        // A gap between the module categories and the general pages, matching the sidebar's two
        // group headings.
        if (index == kModuleCategoryCount) {
            y += kSectionLabelHeight + theme::metrics::kNavSectionGap;
        }

        const Rect row{layout.rail.left() + kNavRowInset, y,
            layout.rail.w - (kNavRowInset * 2.0f), theme::metrics::kNavRowHeight};
        const bool hovered = row.contains(g_state.input.mouse_x, g_state.input.mouse_y);
        g_state.animation.set_target(g_state.nav_hover[index], hovered ? 1.0f : 0.0f);

        if (hovered && clicked) {
            g_state.selected = nav_section(index);
        }
        y += theme::metrics::kNavRowHeight + theme::metrics::kNavRowGap;
    }
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

// ── Step 5: module cards ──────────────────────────────────────────────────────

// One registry pass, reporting the modules of the selected category in display order. Shared by
// input handling and drawing so both always see the same set.
std::size_t visible_modules(modules::BaseModule** out, std::size_t capacity) noexcept {
    const modules::ModuleManager& registry = modules::manager();
    const modules::Category category = modules::category_from_section(g_state.selected);
    std::size_t count = 0;
    for (std::size_t index = 0; index < registry.count() && count < capacity; ++index) {
        modules::BaseModule* module = registry.at(index);
        if (module != nullptr && module->category() == category) {
            out[count++] = module;
        }
    }
    return count;
}

float card_height_for(float density) noexcept {
    return util::lerp(theme::metrics::kCardHeight, theme::metrics::kRowHeight, density);
}

// Card rectangles for the selected category, laid out exactly as draw_module_cards places them.
// Input handling reads this so a click can never land on a card the frame has not drawn.
void compute_module_cards(const Rect& content_area) noexcept {
    const float density = util::clamp01(g_state.animation.value(g_state.density));
    const float gap = theme::metrics::kTileGap;
    const float grid_width = (content_area.w - gap) * 0.5f;
    const float tile_width = util::lerp(grid_width, content_area.w, density);
    const float tile_height = card_height_for(density);

    modules::BaseModule* visible[modules::ModuleManager::kMaxModules] = {};
    const std::size_t count = visible_modules(visible, modules::ModuleManager::kMaxModules);

    float x = content_area.left();
    float y = content_area.top();
    float column = 0.0f;
    std::size_t card = 0;

    for (std::size_t index = 0; index < count; ++index) {
        g_state.card_rects[card] = Rect{x, y, tile_width, tile_height};
        ++card;

        column += 1.0f;
        if (density >= 0.999f) {
            y += tile_height + gap;
            continue;
        }
        if (column >= 2.0f) {
            column = 0.0f;
            x = content_area.left();
            y += tile_height + gap;
        } else {
            x += tile_width + gap;
        }
        if (y + tile_height > content_area.bottom()) {
            break;
        }
    }
    g_state.card_count = card;
}

void handle_module_cards(const Rect& content_area) noexcept {
    compute_module_cards(content_area);
    if (g_state.card_count == 0) {
        return;
    }

    modules::BaseModule* visible[modules::ModuleManager::kMaxModules] = {};
    (void)visible_modules(visible, modules::ModuleManager::kMaxModules);
    const Input& input = g_state.input;

    for (std::size_t index = 0; index < g_state.card_count; ++index) {
        const Rect& card = g_state.card_rects[index];
        if (!card.contains(input.mouse_x, input.mouse_y)) {
            continue;
        }
        // The pill owns the right edge of the card; a press there toggles through it, a press on
        // the card body toggles directly. All mutation happens in this input phase (7.3: render
        // never mutates) and draw_module_cards only reads the resulting state.
        const Rect pill_area = Rect{card.right() - kPillWidth - kCardPadding,
            card.center_y() - (theme::metrics::kPillHeight * 0.5f), kPillWidth,
            theme::metrics::kPillHeight};
        const bool on_pill = pill_area.contains(input.mouse_x, input.mouse_y);
        if (!input.is_pressed(0) || (!card.contains(input.mouse_x, input.mouse_y) && !on_pill)) {
            continue;
        }
        modules::BaseModule* module = visible[index];
        if (module != nullptr && module->set_enabled(!module->enabled())) {
            modules::manager().notify_changed();
            (void)save_config();
        }
    }
}

// Draws the Apple-blue pill switch for one card from the theme primitives. One animated t drives
// both the knob position (lerp) and the background colour (dark gray -> accent cross-fade), so the
// two cannot desync - they are the same number (§7.4).
void draw_pill(ImDrawList* draw_list, const Rect& area, float t, float alpha) noexcept {
    const Rgba background =
        util::mix(theme::color::kCardBorder, theme::color::kAccent, util::clamp01(t));
    draw::rounded_rect(draw_list, area, util::with_alpha(background, alpha), area.h * 0.5f);

    const float knob_radius = theme::metrics::kKnobRadius;
    const float knob_inset = 2.0f;
    const float knob_x = util::lerp(area.left() + knob_radius + knob_inset,
        area.right() - knob_radius - knob_inset, util::clamp01(t));
    draw::circle(draw_list, knob_x, area.center_y(), knob_radius,
        util::with_alpha(util::kWhite, alpha));
}

void draw_module_cards(ImDrawList* draw_list, const Rect& area, float alpha) noexcept {
    if (area.empty()) {
        return;
    }
    // Rectangles come from compute_module_cards(), run in this frame's input phase, so layout
    // and hit-testing are the same numbers by construction.
    modules::BaseModule* visible[modules::ModuleManager::kMaxModules] = {};
    const std::size_t count = visible_modules(visible, modules::ModuleManager::kMaxModules);
    const Input& input = g_state.input;

    for (std::size_t index = 0; index < count && index < g_state.card_count; ++index) {
        const Rect& card = g_state.card_rects[index];
        if (card.empty()) {
            continue;
        }
        modules::BaseModule* module = visible[index];
        if (module == nullptr) {
            continue;
        }

        const bool hovered = card.contains(input.mouse_x, input.mouse_y);
        const bool enabled = module->enabled();

        // Targets are steered here and advanced once per frame by tick_animation, exactly like
        // the sidebar's nav rows (§7.3: animate never draws, render never mutates).
        const StateHandle hover = g_state.card_hover[index];
        const StateHandle pill = g_state.card_pill[index];
        if (hover.valid()) {
            g_state.animation.set_target(hover, hovered ? 1.0f : 0.0f);
        }
        if (pill.valid()) {
            g_state.animation.set_target(pill, enabled ? 1.0f : 0.0f);
        }

        const float hover_amount =
            hover.valid() ? util::clamp01(g_state.animation.value(hover)) : (hovered ? 1.0f : 0.0f);
        const Rgba base = util::mix(theme::color::kCard, theme::color::kCardHover, hover_amount);

        draw::rounded_rect(draw_list, card, util::with_alpha(base, alpha),
            theme::metrics::kFrameRounding);
        draw::border_stroke(draw_list, card,
            util::with_alpha(enabled ? theme::color::kAccent : theme::color::kCardBorder, alpha),
            theme::metrics::kFrameRounding);

        const Rect inner = card.inset(kCardPadding);
        const float text_width = inner.w - (kPillWidth + kCardPadding);
        draw::text_clipped(draw_list,
            Rect{inner.left(), inner.top(), text_width, inner.h * 0.5f}, module->name(),
            util::with_alpha(theme::color::kText, alpha), Align::Left);
        draw::text_clipped(draw_list,
            Rect{inner.left(), inner.top() + kCardDescOffset, text_width, inner.h - kCardDescOffset},
            module->description(), util::with_alpha(theme::color::kTextMuted, alpha), Align::Left,
            theme::metrics::kFooterFontSize);

        // Bind badge, when the module carries one.
        if (module->bind() != 0) {
            const Rect badge = Rect{inner.right() - kBindBadgeWidth,
                inner.top() - kBindBadgeOffset, kBindBadgeWidth, kBindBadgeHeight};
            draw::rounded_rect(draw_list, badge,
                util::with_alpha(theme::color::kNavActive, alpha), 4.0f);
            FixedString<8> bind_text;
            bind_text.format("0x%02X", static_cast<unsigned int>(module->bind()));
            draw::text_in(draw_list, badge, bind_text.c_str(),
                util::with_alpha(theme::color::kTextMuted, alpha), Align::Center,
                theme::metrics::kFooterFontSize);
        }

        const Rect pill_area = Rect{card.right() - kPillWidth - kCardPadding,
            card.center_y() - (theme::metrics::kPillHeight * 0.5f), kPillWidth,
            theme::metrics::kPillHeight};
        const float pill_t =
            pill.valid() ? util::clamp01(g_state.animation.value(pill)) : (enabled ? 1.0f : 0.0f);
        // Draw only: the click that produced this state was consumed in this frame's input
        // phase (handle_module_cards), so the animation target set above is the only write.
        draw_pill(draw_list, pill_area, pill_t, alpha);
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

void draw_sidebar(ImDrawList* draw_list, const Layout& layout, float collapse) noexcept {
    if (collapse >= 0.999f) {
        return;
    }
    const float alpha = 1.0f - collapse;
    draw::rounded_rect(draw_list, layout.rail, util::with_alpha(theme::color::kSidebar, alpha),
        theme::metrics::kWindowRounding, ImDrawFlags_RoundCornersBottomLeft);

    // Logo block: the client name over its version, both clipped to the rail.
    Rect logo{layout.rail.left() + kNavRowInset, layout.rail.top() + 18.0f,
        layout.rail.w - (kNavRowInset * 2.0f), theme::metrics::kTitleFontSize};
    draw::text(draw_list, logo.left(), logo.top(), version::kClientName,
        util::with_alpha(theme::color::kText, alpha), theme::metrics::kTitleFontSize);
    draw::text(draw_list, logo.left(), logo.top() + theme::metrics::kTitleFontSize + 4.0f,
        version::kVersion, util::with_alpha(theme::color::kTextDim, alpha),
        theme::metrics::kFooterFontSize);

    const Rgba section_color = util::with_alpha(theme::color::kTextDim, alpha);
    auto draw_section_label = [&](float y, const char* text) {
        draw::text(draw_list, layout.rail.left() + kNavLabelInset, y, text, section_color,
            theme::metrics::kFooterFontSize);
    };

    float y = layout.rail.top() + kLogoBlockHeight;
    draw_section_label(y + 4.0f, "MODULES");
    y += kSectionLabelHeight;

    for (std::size_t index = 0; index < kNavCount; ++index) {
        if (index == kModuleCategoryCount) {
            y += kSectionLabelHeight + theme::metrics::kNavSectionGap;
            draw_section_label(y - kSectionLabelHeight - 4.0f, "GENERAL");
        }

        const Section section = nav_section(index);
        const Rect row{layout.rail.left() + kNavRowInset, y, layout.rail.w - (kNavRowInset * 2.0f),
            theme::metrics::kNavRowHeight};
        const float hover = util::clamp01(g_state.animation.value(g_state.nav_hover[index]));
        const bool selected = (section == g_state.selected);

        if (selected) {
            draw::rounded_rect(draw_list, row,
                util::with_alpha(theme::color::kNavActive, alpha),
                theme::metrics::kFrameRounding);
            draw::rounded_rect(draw_list,
                Rect{row.left(), row.top() + 5.0f, kNavAccentWidth, row.h - 10.0f},
                util::with_alpha(theme::category_accent(section), alpha),
                kNavAccentWidth * 0.5f);
        } else if (hover > 0.01f) {
            draw::rounded_rect(draw_list, row,
                util::with_alpha(util::mix(theme::color::kCard, theme::color::kCardHover, hover),
                    alpha * 0.7f),
                theme::metrics::kFrameRounding);
        }

        const Rgba label_color = selected ? theme::color::kText
                                          : util::mix(theme::color::kTextMuted, theme::color::kText, hover);
        draw::text_clipped(draw_list, row.inset(kNavLabelInset), section_label(section),
            util::with_alpha(label_color, alpha), Align::Left);

        y += theme::metrics::kNavRowHeight + theme::metrics::kNavRowGap;
    }

    // Footer: build identity plus the current frame rate, so the overlay reports the same numbers
    // the diagnostics tiles do.
    const hooks::game_thread::Stats pipeline = hooks::game_thread::stats();
    FixedString<64> footer;
    footer.format("%s %s | %.0f fps", version::kClientName, version::kVersion,
        static_cast<double>(pipeline.frames_per_second));
    draw::text(draw_list, layout.rail.left() + kNavLabelInset,
        layout.rail.bottom() - kFooterHeight, footer.c_str(),
        util::with_alpha(theme::color::kTextDim, alpha), theme::metrics::kFooterFontSize);
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
    draw::text_in(draw_list, area, "No modules registered in this category yet.",
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

    const Rect body = layout.content.inset(theme::metrics::kContentPadding);
    draw::text(draw_list, body.left(), body.top(), section_label(g_state.selected),
        util::with_alpha(theme::color::kText, alpha), theme::metrics::kHeaderFontSize);

    const Rgba accent =
        util::with_alpha(theme::category_accent(g_state.selected), alpha);
    const float title_bottom = body.top() + theme::metrics::kHeaderFontSize + 6.0f;
    draw::line(draw_list, body.left(), title_bottom, body.left() + 42.0f, title_bottom, accent, 2.0f);
    draw::text_clipped(draw_list,
        Rect{body.left(), title_bottom + 6.0f, body.w, 18.0f}, section_subtitle(g_state.selected),
        util::with_alpha(theme::color::kTextMuted, alpha), Align::Left,
        theme::metrics::kFooterFontSize);

    const Rect content_area =
        Rect{body.left(), title_bottom + 32.0f, body.w, body.bottom() - title_bottom - 32.0f};
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
            draw_module_cards(draw_list, content_area, alpha);
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
}

void compose_frame() noexcept {
    const float open = util::clamp01(g_state.animation.value(g_state.open_spring));
    const float collapse = util::clamp01(g_state.animation.value(g_state.collapse));
    const Layout layout = compute_layout(open, collapse);

    g_state.header = layout.header;
    g_state.density_button = Rect{layout.header.right() - kDensityButtonWidth - 16.0f,
        layout.header.center_y() - (kDensityButtonHeight * 0.5f), kDensityButtonWidth,
        kDensityButtonHeight};

    // Input first, so a click is visible in the frame that produced it.
    handle_lights(layout);
    handle_density_button();
    handle_sidebar_nav(layout, collapse);
    if (collapse < 0.999f && is_module_category(g_state.selected)) {
        // Exactly the rectangle draw_content hands to draw_module_cards: the padding box below
        // the heading and its subtitle. Input handling and drawing must never disagree about
        // where a card is.
        const Rect body = layout.content.inset(theme::metrics::kContentPadding);
        const float header_drop = kTileHeaderOffset + theme::metrics::kHeaderFontSize + 6.0f;
        const Rect card_area{body.left(), body.top() + header_drop, body.w,
            body.h > header_drop ? body.h - header_drop : 0.0f};
        handle_module_cards(card_area);
    }
    handle_drag(layout);

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
        draw_sidebar(draw_list, layout, collapse);
        draw_content(draw_list, layout, collapse);
        draw_chrome_bar(draw_list, layout, collapse);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

// ── Events ────────────────────────────────────────────────────────────────────────

// Deliberately not noexcept: it is passed as a non-type template argument to the bus, and a
// pointer-to-noexcept-function argument for a plain function-pointer parameter is a conformance
// area worth not depending on.
void on_key_event(events::KeyEvent& event) {
    if (!event.down || event.repeat) {
        return;
    }
    if (event.virtual_key != g_state.toggle_key) {
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
    for (std::size_t index = 0; index < kNavCount; ++index) {
        g_state.nav_hover[index] = g_state.animation.acquire_state(0.0f, 18.0f);
    }
    g_state.lights.bind(g_state.animation);

    // One hover + one pill slot per registry row, acquired once so a card never animates from a
    // recycled handle (§7.4). A full pool degrades gracefully: the card falls back to an
    // instantaneous state instead of failing to render.
    for (std::size_t index = 0; index < modules::ModuleManager::kMaxModules; ++index) {
        g_state.card_hover[index] = g_state.animation.acquire_state(0.0f, 18.0f);
        g_state.card_pill[index] = g_state.animation.acquire_state(0.0f, 18.0f);
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

    g_state.lights.unbind();
    for (std::size_t index = 0; index < modules::ModuleManager::kMaxModules; ++index) {
        g_state.animation.release(g_state.card_hover[index]);
        g_state.animation.release(g_state.card_pill[index]);
        g_state.card_hover[index] = animation::StateHandle{};
        g_state.card_pill[index] = animation::StateHandle{};
    }

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
    return !g_state.animation.at_rest(g_state.open_spring);
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
