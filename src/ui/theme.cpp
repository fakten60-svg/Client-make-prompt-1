#include "ui/theme.h"

// The functions here are the ones that cannot be a constexpr token: a table lookup, a label and a
// switch. Keeping them out of the header means the navigation table has exactly one definition, so
// a new section cannot be added to the sidebar without adding its label.

namespace woke::ui {
namespace {

struct SectionInfo {
    const char* label;
    const char* subtitle;
    std::size_t catalog_size; // modules the blueprint's catalogue (§8) assigns to the category
};

// Indexed by Section. The first six entries are the module categories, in the order the sidebar
// lists them; the rest are the general pages.
constexpr SectionInfo kSections[kSectionCount] = {
    {"Combat", "Melee reach, hit feedback and cooldown readouts", 4},
    {"Mace", "Smash potential, wind charge and mace session counters", 4},
    {"Misc", "Client controls: ClickGUI, configs, friends, sound, panic", 5},
    {"Movement", "Client-side sprint and edge-walk assistance", 3},
    {"Spear", "Trident, riptide and loyalty readouts", 3},
    {"Visual", "Rendering-side helpers: fullbright, HUD, zoom, overlays", 5},
    {"Diagnostics", "Live subsystem state read from the client's own counters", 0},
    {"Settings", "Client-wide behaviour and performance options", 0},
    {"Theme", "Accent, backdrop and rounding tokens", 0},
    {"Configs", "Named configuration slots and hotkeys", 0},
    {"Socials", "Local friend list and nametag overrides", 0},
    {"Keybinds", "Every module bind in one place", 0},
};

static_assert(sizeof(kSections) / sizeof(kSections[0]) == kSectionCount,
    "the navigation table and the Section enum must stay in step");

constexpr std::size_t index_of(Section section) noexcept {
    return static_cast<std::size_t>(section);
}

constexpr std::size_t clamp_index(Section section) noexcept {
    return index_of(section) < kSectionCount ? index_of(section) : 0;
}

} // namespace

bool is_module_category(Section section) noexcept {
    return index_of(section) < kModuleCategoryCount;
}

const char* section_label(Section section) noexcept {
    return kSections[clamp_index(section)].label;
}

const char* section_subtitle(Section section) noexcept {
    return kSections[clamp_index(section)].subtitle;
}

std::size_t category_catalog_size(Section section) noexcept {
    return is_module_category(section) ? kSections[clamp_index(section)].catalog_size : 0;
}

namespace theme {

StyleTokens style_tokens() noexcept {
    StyleTokens tokens;
    tokens.window_padding = 0.0f; // the composition pads explicitly, per region
    tokens.frame_padding_x = 10.0f;
    tokens.frame_padding_y = 6.0f;
    tokens.item_spacing_x = 8.0f;
    tokens.item_spacing_y = 6.0f;
    tokens.window_rounding = metrics::kWindowRounding;
    tokens.frame_rounding = metrics::kFrameRounding;
    tokens.window_border_size = metrics::kWindowBorderSize;
    tokens.scrollbar_size = 8.0f;
    tokens.grab_rounding = 6.0f;
    tokens.tab_rounding = metrics::kFrameRounding;
    return tokens;
}

Rgba traffic_light(std::size_t index) noexcept {
    switch (index) {
    case 0:
        return color::kTrafficRed;
    case 1:
        return color::kTrafficYellow;
    case 2:
        return color::kTrafficGreen;
    default:
        return color::kTextDim;
    }
}

Rgba category_accent(Section section) noexcept {
    switch (section) {
    case Section::Combat:
        return util::from_hex(0xFF6B6B);
    case Section::Mace:
        return util::from_hex(0xFFB454);
    case Section::Misc:
        return util::from_hex(0x9B8CFF);
    case Section::Movement:
        return util::from_hex(0x4ADE80);
    case Section::Spear:
        return color::kAccentAlt;
    case Section::Visual:
        return color::kAccent;
    default:
        return color::kTextMuted;
    }
}

} // namespace theme
} // namespace woke::ui
