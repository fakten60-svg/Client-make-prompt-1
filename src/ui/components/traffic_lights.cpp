#include "ui/components/traffic_lights.h"

#include "ui/theme.h"
#include "utils/math_utils.h"

namespace woke::ui::components {
namespace {

using util::Rgba;

constexpr float kHitPadding = 3.0f; // the click target is slightly larger than the glyph
constexpr float kGlyph = 3.0f;      // half-extent of the × / − / + glyph strokes
constexpr float kPressRadiusScale = 0.86f;
constexpr float kHoverRingAlpha = 0.55f;
constexpr float kGlyphAlpha = 0.85f;

} // namespace

void TrafficLights::bind(animation::AnimationController& controller) noexcept {
    controller_ = &controller;
    for (std::size_t index = 0; index < kCount; ++index) {
        hover_[index] = controller.acquire_state(0.0f, 16.0f);
        press_[index] = controller.acquire_state(0.0f, 22.0f);
        hovered_[index] = false;
    }
    armed_ = -1;
    pending_ = Action::None;
}

void TrafficLights::unbind() noexcept {
    if (controller_ == nullptr) {
        return;
    }
    for (std::size_t index = 0; index < kCount; ++index) {
        controller_->release(hover_[index]);
        controller_->release(press_[index]);
        hover_[index] = animation::StateHandle{};
        press_[index] = animation::StateHandle{};
    }
    controller_ = nullptr;
}

void TrafficLights::set_area(const Rect& area) noexcept {
    area_ = area;
}

Rect TrafficLights::button_rect(const Rect& area, std::size_t index) const noexcept {
    const float radius = theme::metrics::kTrafficLightRadius;
    const float center_x =
        area.left() + radius + (static_cast<float>(index) * theme::metrics::kTrafficLightGap);
    const float center_y = area.center_y();
    return Rect{center_x - radius, center_y - radius, radius * 2.0f, radius * 2.0f};
}

int TrafficLights::hit_test(const Rect& area, float x, float y) const noexcept {
    for (std::size_t index = 0; index < kCount; ++index) {
        const Rect button = button_rect(area, index);
        if (button.inset(-kHitPadding).contains(x, y)) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

TrafficLights::Action TrafficLights::action_for(std::size_t index) noexcept {
    switch (index) {
    case 0:
        return Action::Close;
    case 1:
        return Action::Minimize;
    case 2:
        return Action::Zoom;
    default:
        return Action::None;
    }
}

bool TrafficLights::handle_input(const Input& input) noexcept {
    const int hit = hit_test(area_, input.mouse_x, input.mouse_y);

    // Hover is recorded here rather than read in render(), so render() stays a pure draw and the
    // hover animation is driven by the same frame the pointer moved.
    for (std::size_t index = 0; index < kCount; ++index) {
        hovered_[index] = (hit == static_cast<int>(index));
    }

    if (input.is_pressed(0)) {
        armed_ = hit;
        // A press that starts on a button belongs to the button: consume it so the header's drag
        // hit test does not also start a window move.
        return hit >= 0;
    }

    if (input.is_released(0)) {
        const int releasing = armed_;
        armed_ = -1;
        if (releasing >= 0 && releasing == hit) {
            pending_ = action_for(static_cast<std::size_t>(releasing));
            return true;
        }
    }

    // Hovering alone never consumes a click.
    return false;
}

void TrafficLights::animate(float delta_seconds) noexcept {
    if (controller_ == nullptr) {
        return;
    }
    // Targets only: the GUI owns the single AnimationController::tick() for the frame, so a
    // component can never advance the shared clock a second time. `delta_seconds` is accepted to
    // keep the contract's signature and is unused here on purpose.
    (void)delta_seconds;
    for (std::size_t index = 0; index < kCount; ++index) {
        controller_->set_target(hover_[index], hovered_[index] ? 1.0f : 0.0f);
        controller_->set_target(press_[index], armed_ == static_cast<int>(index) ? 1.0f : 0.0f);
    }
}

void TrafficLights::render(ImDrawList* draw_list, const Rect& area) noexcept {
    if (draw_list == nullptr) {
        return;
    }

    for (std::size_t index = 0; index < kCount; ++index) {
        const Rect button = button_rect(area, index);
        const float hover = hover_amount(index);
        const float press = press_amount(index);

        const Rgba base = theme::traffic_light(index);
        const Rgba lit = util::mix(base, util::kWhite, theme::metrics::kHoverMix * hover);
        const float radius =
            theme::metrics::kTrafficLightRadius * util::lerp(1.0f, kPressRadiusScale, press);

        draw::circle(draw_list, button.center_x(), button.center_y(), radius, lit);

        if (hover > 0.01f) {
            draw::ring(draw_list, button.center_x(), button.center_y(), radius + 2.0f,
                util::scale_alpha(util::kWhite, kHoverRingAlpha * hover), 1.0f);
        }

        // The glyph only appears on hover, exactly like the real control.
        if (hover <= 0.05f) {
            continue;
        }

        const Rgba glyph = util::with_alpha(theme::color::kWindowBackdrop, kGlyphAlpha * hover);
        const float cx = button.center_x();
        const float cy = button.center_y();
        switch (index) {
        case 0: // close: ×
            draw::line(draw_list, cx - kGlyph, cy - kGlyph, cx + kGlyph, cy + kGlyph, glyph);
            draw::line(draw_list, cx - kGlyph, cy + kGlyph, cx + kGlyph, cy - kGlyph, glyph);
            break;
        case 1: // minimize: −
            draw::line(draw_list, cx - kGlyph, cy, cx + kGlyph, cy, glyph);
            break;
        default: // zoom: +
            draw::line(draw_list, cx - kGlyph, cy, cx + kGlyph, cy, glyph);
            draw::line(draw_list, cx, cy - kGlyph, cx, cy + kGlyph, glyph);
            break;
        }
    }
}

float TrafficLights::hover_amount(std::size_t index) const noexcept {
    return (controller_ != nullptr && index < kCount) ? controller_->value(hover_[index]) : 0.0f;
}

float TrafficLights::press_amount(std::size_t index) const noexcept {
    return (controller_ != nullptr && index < kCount) ? controller_->value(press_[index]) : 0.0f;
}

TrafficLights::Action TrafficLights::take_action() noexcept {
    const Action action = pending_;
    pending_ = Action::None;
    return action;
}

} // namespace woke::ui::components
