#pragma once

// Sidebar (blueprint §3.6, §7.1) - roadmap step 6.
//
// Logo block, the two navigation groups (MODULES: the six categories with live counter badges;
// GENERAL: Diagnostics / Settings / Theme / Configs / Socials / Keybinds) and the footer. It owns
// both its drawing and its hit-testing, which is the point of §7.3: the row a click lands on and
// the row that is drawn come from the same function, so they cannot disagree.
//
// Counter badges are pushed in from the module registry (enabled, total) rather than read here, so
// the component has no dependency on modules and stays testable on the host.
//
// Portable (no windows.h): theme tokens, fixed text and the shared animation controller only.

#include <array>
#include <cstddef>

#include "ui/animation/animation_controller.h"
#include "ui/components/base_component.h"
#include "ui/theme.h"
#include "utils/string_buffer.h"

namespace woke::ui::components {

class Sidebar final : public BaseUIComponent {
public:
    static constexpr float kLogoBlockHeight = 78.0f;
    static constexpr float kSectionLabelHeight = 22.0f;
    static constexpr float kRowInset = 10.0f;
    static constexpr float kRowLabelInset = 14.0f;
    static constexpr float kAccentWidth = 3.0f;
    static constexpr float kFooterHeight = 26.0f;

    void bind(animation::AnimationController& controller) noexcept;
    void unbind() noexcept;

    void set_area(const Rect& area) noexcept;
    void set_selected(Section section) noexcept;
    [[nodiscard]] Section selected() const noexcept { return selected_; }

    // Counter badge for one navigation row, by Section index. The general pages pass total == 0 and
    // get no badge.
    void set_counts(std::size_t index, std::size_t enabled, std::size_t total) noexcept;
    [[nodiscard]] std::size_t badge_enabled(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t badge_total(std::size_t index) const noexcept;

    void set_footer(const char* text) noexcept;
    void set_alpha(float alpha) noexcept;

    void render(ImDrawList* draw_list, const Rect& area) noexcept override;
    bool handle_input(const Input& input) noexcept override;
    void animate(float delta_seconds) noexcept override;

    // One-shot: the selection changed during the last input frame.
    [[nodiscard]] bool take_selection_changed() noexcept;

    [[nodiscard]] float hover_amount(std::size_t index) const noexcept;

    // Row geometry, computed from the area alone so input, drawing and the host tests share one
    // definition. `index` is a Section index; the two group headings contribute their own space.
    [[nodiscard]] static float row_top(const Rect& area, std::size_t index) noexcept;
    [[nodiscard]] static Rect row_rect(const Rect& area, std::size_t index) noexcept;
    // The group heading a row belongs to ("MODULES"), or "GENERAL" for the rows after the module
    // categories. Returns nullptr for any index outside the table.
    [[nodiscard]] static const char* heading_for(std::size_t index) noexcept;

private:
    animation::AnimationController* controller_ = nullptr;
    std::array<animation::StateHandle, kSectionCount> hover_{};
    std::array<std::size_t, kSectionCount> enabled_{};
    std::array<std::size_t, kSectionCount> total_{};
    Rect area_{};
    util::FixedString<64> footer_{};
    Section selected_ = Section::Diagnostics;
    bool changed_ = false;
    float alpha_ = 1.0f;
};

} // namespace woke::ui::components
