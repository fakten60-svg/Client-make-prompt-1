#pragma once

// Search bar (blueprint §3.6, §7.1) - roadmap step 6.
//
// A 64-char fixed-buffer input that filters the module list. It is a component rather than an
// ImGui::InputText call for three reasons: the buffer cannot grow (§6.3), focus is ours (the game
// owns the OS keyboard, so ImGui's own text input is not reliable inside the overlay), and the
// match predicate is a pure function the host tests can exercise.
//
// Typed characters arrive through push_char()/backspace(), which the GUI feeds from WM_CHAR. The
// component never reads the OS keyboard itself.

#include <cstddef>

#include "ui/animation/animation_controller.h"
#include "ui/components/base_component.h"
#include "utils/string_buffer.h"

namespace woke::ui::components {

class SearchBar final : public BaseUIComponent {
public:
    // "woke.wtf" module names are short; 64 characters is far more than a filter ever needs, and a
    // query longer than this cannot match anything either way.
    static constexpr std::size_t kCapacity = 64;

    void bind(animation::AnimationController& controller) noexcept;
    void unbind() noexcept;

    void set_area(const Rect& area) noexcept;
    void set_focused(bool focused) noexcept;
    void set_alpha(float alpha) noexcept;

    // Text input, routed from the window procedure. Both report whether the text changed, which is
    // what lets the GUI recompute the filtered module list only on a real change (§7.1).
    bool push_char(unsigned int codepoint) noexcept;
    bool backspace() noexcept;

    void clear() noexcept;
    void set_text(const char* text) noexcept;

    [[nodiscard]] const char* text() const noexcept { return buffer_.c_str(); }
    [[nodiscard]] bool empty() const noexcept { return buffer_.empty(); }
    [[nodiscard]] std::size_t length() const noexcept { return buffer_.size(); }
    [[nodiscard]] bool focused() const noexcept { return focused_; }

    // Case-insensitive substring match. An empty query matches everything - that is the "no filter"
    // state, not a special case for the caller to remember.
    [[nodiscard]] bool matches(const char* haystack) const noexcept;

    void render(ImDrawList* draw_list, const Rect& area) noexcept override;
    bool handle_input(const Input& input) noexcept override;
    void animate(float delta_seconds) noexcept override;

    // One-shot: the text changed during the last input/text phase.
    [[nodiscard]] bool take_changed() noexcept;

    [[nodiscard]] float focus_amount() const noexcept;

private:
    animation::AnimationController* controller_ = nullptr;
    animation::StateHandle focus_{};
    animation::StateHandle hover_{};
    Rect area_{};
    util::FixedString<kCapacity> buffer_{};
    bool focused_ = false;
    bool hovered_ = false;
    bool changed_ = false;
    float alpha_ = 1.0f;
};

} // namespace woke::ui::components
