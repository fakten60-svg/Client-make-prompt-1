#pragma once

// Module card (blueprint §3.6, §7.1) - roadmap step 6.
//
// The slate card every module is listed as: white title, muted description, keybind chip, an eased
// chevron for the settings drawer and a right-aligned Apple-style pill. One component, used
// identically in grid and list density, which is what makes the two layouts impossible to diverge.
//
// Two deliberate design choices:
//
//   * The card is the single source of truth for its *appearance*; the module registry is the
//     single source of truth for its *state*. The card never toggles itself - handle_input()
//     reports what the user did and the GUI applies it to the module, then feeds the result back
//     through set_enabled(). The pill therefore cannot disagree with the module.
//   * The chevron rotation and the drawer reveal are one animated value (§7.4). A chevron that
//     says "closed" over an open drawer is not a state this component can represent.
//
// Portable (no windows.h): the host tests drive the whole card - hover, click, expand, capture -
// against a headless ImGui context.
//
// The drawer's *contents* are the GUI's job: this component owns the drawer shell and its reveal,
// while the GUI draws the setting rows it knows about inside drawer_area().

#include <cstddef>

#include "ui/animation/animation_controller.h"
#include "ui/components/base_component.h"
#include "ui/components/keybind_badge.h"
#include "ui/components/pill_toggle.h"
#include "utils/string_buffer.h"

namespace woke::ui::components {

class ModuleCard final : public BaseUIComponent {
public:
    // What the user did this input frame. The GUI applies each flag to the module; the card itself
    // mutates nothing but its own animation targets.
    struct Result {
        bool toggled = false;
        bool expand_toggled = false;
        bool bind_clicked = false;

        [[nodiscard]] bool any() const noexcept {
            return toggled || expand_toggled || bind_clicked;
        }
    };

    static constexpr float kPaddingX = 12.0f;
    static constexpr float kPaddingY = 6.0f;
    static constexpr float kPillWidth = 40.0f;
    static constexpr float kBadgeWidth = 60.0f;
    static constexpr float kBadgeHeight = 18.0f;
    static constexpr float kChevronSize = 16.0f;
    static constexpr float kTitleHeight = 18.0f;
    static constexpr float kDrawerRowHeight = 22.0f;
    static constexpr float kDrawerPadding = 6.0f;

    void bind(animation::AnimationController& controller) noexcept;
    void unbind() noexcept;

    void set_area(const Rect& area) noexcept;
    void set_content(const char* title, const char* description, int bind_key) noexcept;
    void set_enabled(bool enabled) noexcept;
    void set_expanded(bool expanded) noexcept;
    // How many setting rows the drawer will hold; the shell sizes itself from this. Zero means the
    // chevron stays a static affordance and no drawer is drawn.
    void set_drawer_rows(std::size_t rows) noexcept;
    void set_capture_armed(bool armed) noexcept;
    void set_alpha(float alpha) noexcept;

    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool expanded() const noexcept { return expanded_; }
    [[nodiscard]] std::size_t drawer_rows() const noexcept { return drawer_rows_; }
    [[nodiscard]] const char* title() const noexcept { return title_.c_str(); }

    // One animated value drives the chevron angle and the drawer reveal (§7.4).
    [[nodiscard]] float open_amount() const noexcept;
    [[nodiscard]] float hover_amount() const noexcept;

    [[nodiscard]] KeybindBadge& badge() noexcept { return badge_; }
    [[nodiscard]] PillToggle& pill() noexcept { return pill_; }

    // Sub-rectangles, in screen space, from the area passed to set_area(). Public so hit-testing,
    // drawing and the host tests all read the same numbers.
    [[nodiscard]] Rect badge_area() const noexcept;
    [[nodiscard]] Rect pill_area() const noexcept;
    [[nodiscard]] Rect chevron_area() const noexcept;
    // Where the drawer sits, at its fully open size. Its visible height is drawer_height() *
    // open_amount(); the GUI multiplies the row alpha by the same value.
    [[nodiscard]] Rect drawer_area() const noexcept;
    [[nodiscard]] float drawer_height() const noexcept;
    // Same two facts for a card the caller has not constructed a component for (the GUI's layout
    // pass sizes drawers before the cards' own areas are set).
    [[nodiscard]] static Rect drawer_area_for(const Rect& card, std::size_t rows) noexcept;
    [[nodiscard]] static float drawer_height_for(std::size_t rows) noexcept;

    // The card's interaction entry point. It returns a Result rather than a bool, so it cannot be
    // the BaseUIComponent::handle_input override (which is typed `bool`): the base contract is
    // satisfied by the thin wrapper below, which reports "something was consumed" for the generic
    // component dispatch, while the GUI - which needs to know *what* happened - calls interact().
    [[nodiscard]] Result interact(const Input& input) noexcept;
    bool handle_input(const Input& input) noexcept override;
    void render(ImDrawList* draw_list, const Rect& area) noexcept override;
    void animate(float delta_seconds) noexcept override;

    // Body height for a layout density (0 = grid tile, 1 = list row). The GUI's layout and the card
    // agree about the collapsed height by calling this one function.
    [[nodiscard]] static float body_height(float density) noexcept;

private:
    animation::AnimationController* controller_ = nullptr;
    animation::StateHandle hover_{};
    animation::StateHandle open_{};
    PillToggle pill_{};
    KeybindBadge badge_{};
    Rect area_{};
    util::FixedString<48> title_{};
    util::FixedString<96> description_{};
    std::size_t drawer_rows_ = 0;
    bool enabled_ = false;
    bool expanded_ = false;
    bool hovered_ = false;
    float alpha_ = 1.0f;
};

} // namespace woke::ui::components
