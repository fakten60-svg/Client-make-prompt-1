#include "ui/components/pill_toggle.h"

#include "ui/theme.h"
#include "utils/math_utils.h"
#include "utils/render_utils.h"

namespace woke::ui::components {
namespace {
namespace draw = woke::ui::draw;
using util::Rgba;

constexpr float kHoverMix = 0.12f;
constexpr float kKnobInset = 2.0f;

} // namespace

void PillToggle::bind(animation::AnimationController& controller) noexcept {
    controller_ = &controller;
    amount_ = controller.acquire_state(0.0f, 18.0f);
}

void PillToggle::unbind() noexcept {
    if (controller_ != nullptr) {
        controller_->release(amount_);
        amount_ = animation::StateHandle{};
    }
    controller_ = nullptr;
}

void PillToggle::set_area(const Rect& area) noexcept {
    area_ = area;
}

void PillToggle::set_value(bool on) noexcept {
    target_on_ = on;
}

void PillToggle::set_alpha(float alpha) noexcept {
    alpha_ = util::clamp01(alpha);
}

bool PillToggle::handle_input(const Input& input) noexcept {
    if (area_.empty()) {
        return false;
    }
    const bool hovered = area_.contains(input.mouse_x, input.mouse_y);
    if (hovered && input.is_pressed(0)) {
        target_on_ = !target_on_;
        clicked_ = true;
        return true; // the click belongs to the pill, not to whatever is underneath
    }
    return false;
}

void PillToggle::animate(float /*delta_seconds*/) noexcept {
    // The GUI owns the single controller tick; components only steer targets.
    if (controller_ != nullptr) {
        controller_->set_target(amount_, target_on_ ? 1.0f : 0.0f);
    }
}

void PillToggle::render(ImDrawList* draw_list, const Rect& area) noexcept {
    if (draw_list == nullptr || area.empty() || controller_ == nullptr) {
        return;
    }

    const float t = util::clamp01(controller_->value(amount_));
    const Rgba off_color = theme::color::kCardBorder;
    const Rgba on_color = theme::color::kAccent;
    const Rgba background = util::mix(off_color, on_color, t);

    draw::rounded_rect(draw_list, area, util::with_alpha(background, alpha_), area.h * 0.5f);

    const float knob_radius = theme::metrics::kKnobRadius;
    const float knob_y = area.center_y();
    const float knob_x =
        util::lerp(area.left() + knob_radius + kKnobInset, area.right() - knob_radius - kKnobInset,
            t);
    draw::circle(draw_list, knob_x, knob_y, knob_radius, util::with_alpha(util::kWhite, alpha_));
}

bool PillToggle::take_click() noexcept {
    const bool clicked = clicked_;
    clicked_ = false;
    return clicked;
}

float PillToggle::amount() const noexcept {
    return controller_ != nullptr ? util::clamp01(controller_->value(amount_)) : 0.0f;
}

} // namespace woke::ui::components
