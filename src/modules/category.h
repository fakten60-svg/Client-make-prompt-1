#pragma once

// Module categories (blueprint §3.4).
//
// The module system and the sidebar speak the same vocabulary through this header: a module is
// registered under a Category, and category_to_section() is the single mapping onto the GUI's
// Section enum. Adding a category therefore has exactly one place to update, and a module can
// never end up registered into a category the sidebar does not render.

#include <cstddef>
#include <cstdint>

#include "ui/theme.h"

namespace woke::modules {

enum class Category : std::uint8_t {
    Combat = 0,
    Mace,
    Misc,
    Movement,
    Spear,
    Visual,
    Count,
};

inline constexpr std::size_t kCategoryCount = static_cast<std::size_t>(Category::Count);

[[nodiscard]] constexpr ui::Section category_to_section(Category category) noexcept {
    return static_cast<ui::Section>(static_cast<std::size_t>(category));
}

[[nodiscard]] constexpr Category category_from_section(ui::Section section) noexcept {
    return static_cast<Category>(static_cast<std::size_t>(section));
}

[[nodiscard]] constexpr bool section_is_category(ui::Section section) noexcept {
    return static_cast<std::size_t>(section) < kCategoryCount;
}

// Inline because it is header-only (forwards to the sidebar's label table); one definition per
// inclusion is expected.
[[nodiscard]] inline const char* category_label(Category category) noexcept {
    return ui::section_label(category_to_section(category));
}

} // namespace woke::modules
