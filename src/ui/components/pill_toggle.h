#pragma once

// Pill toggle (blueprint §3.6, §7.4).
//
// One animated t in [0,1] drives both the knob position (lerp) and the background colour
// (dark gray -> Apple blue cross-fade), so the two can never desync - they are the same number.
// Height 22, knob Ø 18 per §7.2's metrics.

#include <cstddef>

#include "ui/animation/animation_controller.h"
#include "ui/components/base_component.h"

namespace woke::ui::components {

class PillToggle final : public BaseUIComponent {
public:
    void bind(animation::AnimationController& controller) noexcept;
    void unbind() noexcept;

    void set_area(const Rect& area) noexcept;
    void set_value(bool on) noexcept;
    // Opacity, so the switch fades with its card during the ClickGUI's open/close animation.
    void set_alpha(float alpha) noexcept;

    void render(ImDrawList* draw_list, const Rect& area) noexcept override;
    bool handle_input(const Input& input) noexcept override;
    void animate(float delta_seconds) noexcept override;

    // One-shot click result, cleared after read (same pattern as TrafficLights).
    [[nodiscard]] bool take_click() noexcept;

    [[nodiscard]] float amount() const noexcept;
    [[nodiscard]] bool value() const noexcept { return target_on_; }

private:
    animation::AnimationController* controller_ = nullptr;
    animation::StateHandle amount_{};
    Rect area_{};
    bool target_on_ = false;
    bool clicked_ = false;
    float alpha_ = 1.0f;
};

} // namespace woke::ui::components
