#pragma once

// Theme tokens: palette, metrics and navigation table (blueprint §7.1, §7.2).
//
// Every colour and every distance the overlay uses lives here, so a re-theme is one file and no
// widget ever hard-codes a hex value or a magic number. Palette entries are written as hex exactly
// as the blueprint's table writes them, which makes a token reviewable by eye.
//
// The tokens themselves are portable (no windows.h, no imgui.h) and therefore covered by the host
// test suite: `theme::style_tokens()` is the subset of ImGuiStyle the GUI overrides, kept as plain
// numbers here and pushed into ImGui in exactly one place (ui/gui.cpp).

#include <cstddef>
#include <cstdint>

#include "utils/math_utils.h"

namespace woke::ui {

// Sidebar navigation (blueprint §7.1): six module categories, then the general pages.
enum class Section : std::uint8_t {
    Combat = 0,
    Mace,
    Misc,
    Movement,
    Spear,
    Visual,
    Diagnostics,
    Settings,
    ThemePage,
    Configs,
    Socials,
    Keybinds,
    Count,
};

inline constexpr std::size_t kSectionCount = static_cast<std::size_t>(Section::Count);
inline constexpr std::size_t kModuleCategoryCount = 6;

[[nodiscard]] bool is_module_category(Section section) noexcept;

// Row label in the sidebar ("Combat", "Theme") and the content pane's heading.
[[nodiscard]] const char* section_label(Section section) noexcept;

// One-line description for the content pane's sub-badge.
[[nodiscard]] const char* section_subtitle(Section section) noexcept;

// How many modules the blueprint's catalogue (§8) assigns to this category. Shown as the honest
// "planned" figure while a category is still empty; the live count comes from the module registry
// in roadmap step 5.
[[nodiscard]] std::size_t category_catalog_size(Section section) noexcept;

namespace theme {

using util::Rgba;

namespace color {

inline constexpr Rgba kWindowBackdrop = util::from_hex(0x0B0E14, 0.90f);
inline constexpr Rgba kWindowBorder = util::from_hex(0x2A3548);
inline constexpr Rgba kChromeBar = util::from_hex(0x121826);
inline constexpr Rgba kSidebar = util::from_hex(0x0E1420);
inline constexpr Rgba kContent = util::from_hex(0x0D121C);
inline constexpr Rgba kCard = util::from_hex(0x171C28);
inline constexpr Rgba kCardHover = util::from_hex(0x1D2434);
inline constexpr Rgba kCardBorder = util::from_hex(0x232C3E);
inline constexpr Rgba kSeparator = util::from_hex(0x1B2233);
inline constexpr Rgba kNavActive = util::from_hex(0x1A2333);
inline constexpr Rgba kText = util::from_hex(0xFFFFFF);
inline constexpr Rgba kTextMuted = util::from_hex(0x8A96A8);
inline constexpr Rgba kTextDim = util::from_hex(0x5A6579);
inline constexpr Rgba kAccent = util::from_hex(0x0A84FF);
inline constexpr Rgba kAccentAlt = util::from_hex(0x3FD2E0);
inline constexpr Rgba kTrafficRed = util::from_hex(0xFF5F56);
inline constexpr Rgba kTrafficYellow = util::from_hex(0xFFBD2E);
inline constexpr Rgba kTrafficGreen = util::from_hex(0x27C93F);
inline constexpr Rgba kShadow = util::from_hex(0x000000, 0.55f);

} // namespace color

namespace metrics {

inline constexpr float kWindowWidth = 980.0f;
inline constexpr float kWindowHeight = 620.0f;
inline constexpr float kSidebarWidth = 220.0f;
inline constexpr float kHeaderHeight = 64.0f;
inline constexpr float kContentPadding = 20.0f;

inline constexpr float kWindowRounding = 14.0f;
inline constexpr float kFrameRounding = 8.0f;
inline constexpr float kWindowBorderSize = 1.0f;
inline constexpr float kSeparatorThickness = 1.0f;
inline constexpr float kShadowSpread = 14.0f;

inline constexpr float kTrafficLightRadius = 6.0f; // Ø 12
inline constexpr float kTrafficLightGap = 20.0f;
inline constexpr float kTrafficLightInset = 22.0f;

inline constexpr float kNavRowHeight = 30.0f;
inline constexpr float kNavRowGap = 2.0f;
inline constexpr float kNavSectionGap = 18.0f;

inline constexpr float kCardWidth = 340.0f;  // grid tile
inline constexpr float kCardHeight = 92.0f;  // grid tile
inline constexpr float kRowHeight = 46.0f;   // list row
inline constexpr float kTileGap = 16.0f;

inline constexpr float kPillHeight = 22.0f;
inline constexpr float kKnobRadius = 9.0f;

inline constexpr float kOpenScaleFrom = 0.94f;
inline constexpr float kCollapsedWidth = 188.0f;
inline constexpr float kCollapsedHeight = 34.0f;

inline constexpr float kTitleFontSize = 26.0f;
inline constexpr float kHeaderFontSize = 20.0f;
inline constexpr float kFooterFontSize = 12.0f;

inline constexpr float kHoverBrightness = 1.25f;
inline constexpr float kHoverMix = 0.22f; // blend toward white at full hover

} // namespace metrics

// The subset of ImGuiStyle the overlay overrides. Kept as plain numbers so the table is portable
// and testable; gui.cpp is the only place that applies it.
struct StyleTokens {
    float window_padding = 0.0f;
    float frame_padding_x = 10.0f;
    float frame_padding_y = 6.0f;
    float item_spacing_x = 8.0f;
    float item_spacing_y = 6.0f;
    float window_rounding = metrics::kWindowRounding;
    float frame_rounding = metrics::kFrameRounding;
    float window_border_size = metrics::kWindowBorderSize;
    float scrollbar_size = 8.0f;
    float grab_rounding = 6.0f;
    float tab_rounding = metrics::kFrameRounding;
};

[[nodiscard]] StyleTokens style_tokens() noexcept;

// Traffic light colours by index: 0 close, 1 minimize, 2 zoom. Any other index is muted text.
[[nodiscard]] Rgba traffic_light(std::size_t index) noexcept;

// Per-category accent, used for the selected navigation bar and the content heading underline.
[[nodiscard]] Rgba category_accent(Section section) noexcept;

} // namespace theme
} // namespace woke::ui
