#include "ui/components/module_card.h"

#include <cmath>

#include "ui/theme.h"
#include "utils/math_utils.h"

namespace woke::ui::components {
namespace {

using util::Rgba;

constexpr float kChevronThickness = 1.6f;
constexpr float kChevronArm = 4.5f;
constexpr float kBadgeGap = 6.0f;
constexpr float kChevronGap = 6.0f;
constexpr float kTextGap = 8.0f;
constexpr float kDescriptionGap = 2.0f;
constexpr float kHalfPi = 1.57079632f;

[[nodiscard]] Rect padded(const Rect& area) noexcept {
    return Rect{area.left() + ModuleCard::kPaddingX, area.top() + ModuleCard::kPaddingY,
        area.w - (ModuleCard::kPaddingX * 2.0f), area.h - (ModuleCard::kPaddingY * 2.0f)};
}

// A right-pointing chevron that rotates to point down as the drawer opens: one angle, two segments,
// no font glyph and no per-frame path allocation.
void draw_chevron(ImDrawList* draw_list, const Rect& area, float open, const Rgba& color) noexcept {
    const float angle = kHalfPi * util::clamp01(open);
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float cx = area.center_x();
    const float cy = area.center_y();

    auto rotated = [&](float px, float py) -> ImVec2 {
        return ImVec2{cx + (px * c) - (py * s), cy + (px * s) + (py * c)};
    };

    const ImVec2 tip = rotated(kChevronArm, 0.0f);
    const ImVec2 top = rotated(-kChevronArm, -kChevronArm);
    const ImVec2 bottom = rotated(-kChevronArm, kChevronArm);
    draw_list->AddLine(top, tip, draw::to_im_u32(color), kChevronThickness);
    draw_list->AddLine(tip, bottom, draw::to_im_u32(color), kChevronThickness);
}

} // namespace

void ModuleCard::bind(animation::AnimationController& controller) noexcept {
    controller_ = &controller;
    hover_ = controller.acquire_state(0.0f, 18.0f);
    open_ = controller.acquire_state(0.0f, 16.0f);
    pill_.bind(controller);
    badge_.bind(controller);
}

void ModuleCard::unbind() noexcept {
    pill_.unbind();
    badge_.unbind();
    if (controller_ != nullptr) {
        controller_->release(hover_);
        controller_->release(open_);
        hover_ = animation::StateHandle{};
        open_ = animation::StateHandle{};
        controller_ = nullptr;
    }
}

void ModuleCard::set_area(const Rect& area) noexcept {
    area_ = area;
    badge_.set_area(badge_area());
}

void ModuleCard::set_content(
    const char* title, const char* description, int bind_key) noexcept {
    title_.assign(title != nullptr ? title : "");
    description_.assign(description != nullptr ? description : "");
    badge_.set_bind(bind_key);
}

void ModuleCard::set_enabled(bool enabled) noexcept {
    enabled_ = enabled;
}

void ModuleCard::set_expanded(bool expanded) noexcept {
    expanded_ = expanded;
}

void ModuleCard::set_drawer_rows(std::size_t rows) noexcept {
    drawer_rows_ = rows;
}

void ModuleCard::set_capture_armed(bool armed) noexcept {
    badge_.set_capture_armed(armed);
}

void ModuleCard::set_alpha(float alpha) noexcept {
    alpha_ = util::clamp01(alpha);
    pill_.set_alpha(alpha_);
    badge_.set_alpha(alpha_);
}

float ModuleCard::body_height(float density) noexcept {
    return util::lerp(theme::metrics::kCardHeight, theme::metrics::kRowHeight,
        util::clamp01(density));
}

float ModuleCard::drawer_height_for(std::size_t rows) noexcept {
    if (rows == 0) {
        return 0.0f;
    }
    return (kDrawerPadding * 2.0f) + (static_cast<float>(rows) * kDrawerRowHeight);
}

Rect ModuleCard::drawer_area_for(const Rect& card, std::size_t rows) noexcept {
    return Rect{card.left(), card.bottom(), card.w, drawer_height_for(rows)};
}

float ModuleCard::drawer_height() const noexcept {
    return drawer_height_for(drawer_rows_);
}

Rect ModuleCard::pill_area() const noexcept {
    const Rect inner = padded(area_);
    return Rect{inner.right() - kPillWidth, area_.center_y() - (theme::metrics::kPillHeight * 0.5f),
        kPillWidth, theme::metrics::kPillHeight};
}

Rect ModuleCard::chevron_area() const noexcept {
    const Rect pill = pill_area();
    return Rect{pill.left() - kChevronGap - kChevronSize,
        area_.center_y() - (kChevronSize * 0.5f), kChevronSize, kChevronSize};
}

Rect ModuleCard::badge_area() const noexcept {
    const Rect chevron = chevron_area();
    return Rect{chevron.left() - kBadgeGap - kBadgeWidth,
        area_.center_y() - (kBadgeHeight * 0.5f), kBadgeWidth, kBadgeHeight};
}

Rect ModuleCard::drawer_area() const noexcept {
    return Rect{area_.left(), area_.bottom(), area_.w, drawer_height()};
}

float ModuleCard::open_amount() const noexcept {
    return controller_ != nullptr ? util::clamp01(controller_->value(open_)) : 0.0f;
}

float ModuleCard::hover_amount() const noexcept {
    return controller_ != nullptr ? util::clamp01(controller_->value(hover_)) : 0.0f;
}

ModuleCard::Result ModuleCard::interact(const Input& input) noexcept {
    Result result;
    if (area_.empty()) {
        return result;
    }
    hovered_ = area_.contains(input.mouse_x, input.mouse_y);

    // The chip is checked first: a press on it belongs to the chip, never to the card body.
    badge_.set_area(badge_area());
    if (badge_.handle_input(input)) {
        if (badge_.take_click()) {
            result.bind_clicked = true;
        }
        return result;
    }

    if (!hovered_ || !input.is_pressed(0)) {
        return result;
    }
    if (chevron_area().contains(input.mouse_x, input.mouse_y)) {
        result.expand_toggled = true;
    } else {
        // The pill and the card body are one target on purpose: a card whose switch looks clickable
        // but only reacts on half of itself is the usual cause of "the toggle does nothing".
        result.toggled = true;
    }
    return result;
}

bool ModuleCard::handle_input(const Input& input) noexcept {
    return interact(input).any();
}

void ModuleCard::animate(float delta_seconds) noexcept {
    // The pill's value is fed from the module state, never from its own click handling: one source
    // of truth, so the switch and the module cannot drift apart.
    pill_.set_value(enabled_);
    pill_.animate(delta_seconds);
    badge_.animate(delta_seconds);

    if (controller_ == nullptr) {
        return;
    }
    controller_->set_target(hover_, hovered_ ? 1.0f : 0.0f);
    controller_->set_target(open_, expanded_ ? 1.0f : 0.0f);
}

void ModuleCard::render(ImDrawList* draw_list, const Rect& area) noexcept {
    if (draw_list == nullptr || area.empty() || alpha_ <= 0.001f) {
        return;
    }

    const float open = open_amount();
    const float hover = hover_amount();

    // Drawer shell first, so the card body's border sits on top of the seam between them.
    const float drawer_visible = drawer_height() * open;
    if (drawer_visible > 0.5f) {
        const Rect drawer{area.left(), area.bottom(), area.w, drawer_visible};
        draw::rounded_rect(draw_list, drawer, util::with_alpha(theme::color::kSidebar, alpha_),
            theme::metrics::kFrameRounding, ImDrawFlags_RoundCornersBottom);
        draw::border_stroke(draw_list, drawer,
            util::with_alpha(theme::color::kCardBorder, alpha_), theme::metrics::kFrameRounding);
    }

    const Rgba fill = util::mix(theme::color::kCard, theme::color::kCardHover, hover);
    draw::rounded_rect(draw_list, area, util::with_alpha(fill, alpha_),
        theme::metrics::kFrameRounding);
    draw::border_stroke(draw_list, area,
        util::with_alpha(enabled_ ? theme::color::kAccent : theme::color::kCardBorder, alpha_),
        theme::metrics::kFrameRounding);

    const Rect inner = padded(area);
    const float text_width = badge_area().left() - inner.left() - kTextGap;
    if (text_width > 4.0f) {
        draw::text_clipped(draw_list,
            Rect{inner.left(), inner.top(), text_width, kTitleHeight}, title_.c_str(),
            util::with_alpha(theme::color::kText, alpha_), draw::Align::Left);
        const float description_top = inner.top() + kTitleHeight + kDescriptionGap;
        if (inner.bottom() > description_top + 2.0f) {
            draw::text_clipped(draw_list,
                Rect{inner.left(), description_top, text_width, inner.bottom() - description_top},
                description_.c_str(), util::with_alpha(theme::color::kTextMuted, alpha_),
                draw::Align::Left, theme::metrics::kFooterFontSize);
        }
    }

    // The pill and the chip are components in their own right, drawn through their own contract;
    // the card only positions them.
    pill_.render(draw_list, pill_area());
    badge_.render(draw_list, badge_area());

    const Rgba chevron_color =
        util::mix(theme::color::kTextDim, theme::color::kTextMuted, hover);
    draw_chevron(draw_list, chevron_area(), open,
        util::with_alpha(chevron_color, alpha_));
}

} // namespace woke::ui::components
