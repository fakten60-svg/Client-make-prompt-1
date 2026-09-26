#pragma once

// The ClickGUI's shared composition state (blueprint §7.1-§7.8).
//
// gui.cpp used to be one 1200-line translation unit whose halves shared a single anonymous
// namespace; when it grew again the in-world half was split into an included fragment, which
// kept the anonymity but made the file a fragment generator. Step 9 splits it properly into
// three translation units, and this internal header is the contract between them:
//
//   * gui_chrome.cpp - static composition: state, timing, theme, layout, the Diagnostics and
//     Theme pages, and the toasts;
//   * gui_hud.cpp    - the per-frame in-world overlay and HUD composition;
//   * gui_overlay.cpp- the per-frame path, events, lifecycle and the public ui/ overlay API.
//
// The state struct is the one definition of the GUI's bookkeeping. Nothing here is exported
// beyond ui/: this header is internal to the three files above, and every public symbol still
// enters through ui/gui.h.
//
// Windows-only, like the composition it describes.

#ifndef _WIN32
#error "gui_internal.h is Windows-only; portable translation units must not include it."
#endif

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include <imgui.h>

#include "core/event_bus.h"
#include "modules/module_manager.h"
#include "settings/setting.h"
#include "ui/animation/animation_controller.h"
#include "ui/components/module_card.h"
#include "ui/components/search_bar.h"
#include "ui/components/sidebar.h"
#include "ui/components/traffic_lights.h"
#include "ui/gui.h"
#include "ui/notifications.h"
#include "ui/theme.h"
#include "utils/math_utils.h"
#include "utils/render_utils.h"
#include "utils/string_buffer.h"

namespace woke::ui {
namespace detail {

using animation::AnimationController;
using components::Input;
using components::ModuleCard;
using components::SearchBar;
using components::Sidebar;
using components::TrafficLights;
using draw::Rect;
using util::FixedString;
using util::Rgba;

// The Misc/ClickGUI bind (§8: default RSHIFT).
inline constexpr int kDefaultToggleKey = VK_RSHIFT;

// Readouts are recomputed four times a second. The numbers on screen change at human speed, and
// formatting ten labels per frame would be ten snprintf calls of pure waste inside the render
// thread - the overlay's own budget is 0.2 ms (§7.4 gate).
inline constexpr double kReadoutIntervalSeconds = 0.25;

inline constexpr double kChromeWarningCooldownSeconds = 10.0;

inline constexpr std::uint64_t kHeartbeatFrames = 600;
inline constexpr double kMaxFrameDeltaSeconds = 0.25;
inline constexpr std::size_t kReadoutCount = 10;

inline constexpr float kDensityButtonWidth = 62.0f;
inline constexpr float kDensityButtonHeight = 22.0f;
inline constexpr float kTileLabelOffset = 12.0f;
inline constexpr float kTileValueOffset = 32.0f;

// Content-pane metrics the composition owns: the search field in the header row, and the inset of
// a settings row inside an expanded card's drawer. Everything card-shaped (body, drawer, chevron,
// chip, pill) is sized by ModuleCard itself, so there is one definition of a card's geometry.
inline constexpr float kSearchWidth = 220.0f;
inline constexpr float kSearchHeight = 26.0f;
inline constexpr float kSettingRowInset = 12.0f;
inline constexpr float kSettingValueColumn = 96.0f;

// Vertical distance from the content pane's padding box to where the module cards start: the
// heading, its subtitle and one spacer row (matches draw_content's own layout math).
inline constexpr float kTileHeaderOffset = 32.0f;

struct Readout {
    FixedString<24> label;
    FixedString<44> value;
};

// ── The one GUI state ──────────────────────────────────────────────────────────────
//
// Declared here, defined in gui_overlay.cpp (one instance, owned by one TU, so there is no
// static-initialisation-order question between the three files).
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
    animation::StateHandle collapse{};
    animation::StateHandle density{};
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

[[nodiscard]] State& state() noexcept;

// ── Timing (gui_chrome.cpp) ─────────────────────────────────────────────────────────

[[nodiscard]] double now_seconds() noexcept;
[[nodiscard]] float milliseconds_between(const LARGE_INTEGER& start, const LARGE_INTEGER& end) noexcept;

// ── ImGui style (gui_chrome.cpp) ────────────────────────────────────────────────────

void apply_imgui_style() noexcept;

// ── Renderer init (gui_overlay.cpp) ─────────────────────────────────────────────────
//
// ensure_renderer() is deliberately NOT declared here: it is defined in gui_overlay.cpp, the
// one file that owns the ImGui context lifecycle, and the other two never call it. A public
// declaration would collide with the using-directive in that file's public-API half.

// ── Readouts (gui_chrome.cpp) ───────────────────────────────────────────────────────

template <typename... Args>
void set_readout(std::size_t index, const char* label, const char* format, Args... args) noexcept {
    State& s = state();
    if (index >= kReadoutCount) {
        return;
    }
    s.readouts[index].label.assign(label);
    s.readouts[index].value.format(format, args...);
}

void refresh_readouts(double now) noexcept;

// ── Layout and shared geometry (gui_chrome.cpp) ─────────────────────────────────────

struct Layout {
    Rect window{};
    Rect header{};
    Rect body{};
    Rect rail{};
    Rect content{};
};

[[nodiscard]] Layout compute_layout(float open_amount, float collapse) noexcept;
[[nodiscard]] Input snapshot_input() noexcept;

// The content pane's padding box: where the heading, the subtitle and the search field live.
[[nodiscard]] Rect content_body(const Rect& content) noexcept;

// The search field, right-aligned on the heading row. Input handling and drawing both call this,
// so the hit target and the field cannot drift apart.
[[nodiscard]] Rect search_field_rect(const Rect& content) noexcept;

// The rectangle the module cards occupy: the body below the heading, its accent underline and its
// subtitle line. One definition for input and drawing (the step-4 note promised exactly this).
[[nodiscard]] Rect card_pane(const Rect& content) noexcept;

// ── In-world overlay (gui_hud.cpp; consumed by the frame tick) ──────────────────────

void render_inworld_overlay() noexcept;
[[nodiscard]] bool inworld_overlay_wanted() noexcept;

// ── Toasts and the overlay-awake flag (gui_chrome.cpp) ──────────────────────────────

void push_toast(const char* title, const char* message, ToastIcon icon) noexcept;
void push_module_toast(const char* name, bool enabled) noexcept;

// The overlay must stay awake for something that is not the chrome: a toast outlives the ClickGUI
// being hidden, and the frame scheduler's predicate reads this flag. Also mirrors visibility onto
// the ClickGUI module so its pill cannot disagree with the window.
void sync_overlay_request() noexcept;

// ── Frame pieces shared between gui_chrome.cpp and gui_overlay.cpp ──────────────────

void handle_drag(const Layout& layout) noexcept;
void sync_sidebar() noexcept;
void handle_lights(const Layout& layout) noexcept;
void handle_density_button() noexcept;
void handle_module_cards(const Rect& pane, float alpha) noexcept;
void handle_settings_rows() noexcept;
void draw_settings_rows(ImDrawList* draw_list, float alpha) noexcept;
void draw_module_cards(ImDrawList* draw_list) noexcept;

// Rebuilds the filtered module list for the selected category. Lives with the card logic in
// gui_chrome.cpp but runs from gui_overlay.cpp's compose_frame on a dirty list.
void recompute_visible_modules() noexcept;

[[nodiscard]] FixedString<64> compose_title() noexcept;
void draw_chrome_bar(ImDrawList* draw_list, const Layout& layout, float collapse) noexcept;
void draw_content(ImDrawList* draw_list, const Layout& layout, float collapse) noexcept;

// The toast stack, drawn last and outside the window (§7.5): it must survive the chrome being
// hidden, so it renders on the foreground list wherever the frame left it.
void render_notifications_layer() noexcept;

void tick_animation(double now) noexcept;

} // namespace detail
} // namespace woke::ui
