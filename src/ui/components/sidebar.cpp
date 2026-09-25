#include "ui/components/sidebar.h"

#include <cstdio>

#include "core/version.h"
#include "utils/math_utils.h"

namespace woke::ui::components {
namespace {

using util::Rgba;

constexpr float kLogoTop = 18.0f;
constexpr float kLogoLabelGap = 4.0f;
constexpr float kBadgeWidth = 34.0f;
constexpr float kBadgeHeight = 16.0f;
constexpr float kBadgeGap = 6.0f;

[[nodiscard]] float row_stride() noexcept {
    return theme::metrics::kNavRowHeight + theme::metrics::kNavRowGap;
}

} // namespace

void Sidebar::bind(animation::AnimationController& controller) noexcept {
    controller_ = &controller;
    for (std::size_t index = 0; index < kSectionCount; ++index) {
        hover_[index] = controller.acquire_state(0.0f, 18.0f);
    }
}

void Sidebar::unbind() noexcept {
    if (controller_ == nullptr) {
        return;
    }
    for (std::size_t index = 0; index < kSectionCount; ++index) {
        controller_->release(hover_[index]);
        hover_[index] = animation::StateHandle{};
    }
    controller_ = nullptr;
}

void Sidebar::set_area(const Rect& area) noexcept {
    area_ = area;
}

void Sidebar::set_selected(Section section) noexcept {
    if (static_cast<std::size_t>(section) < kSectionCount) {
        selected_ = section;
    }
}

void Sidebar::set_counts(std::size_t index, std::size_t enabled, std::size_t total) noexcept {
    if (index >= kSectionCount) {
        return;
    }
    enabled_[index] = enabled;
    total_[index] = total;
}

std::size_t Sidebar::badge_enabled(std::size_t index) const noexcept {
    return index < kSectionCount ? enabled_[index] : 0;
}

std::size_t Sidebar::badge_total(std::size_t index) const noexcept {
    return index < kSectionCount ? total_[index] : 0;
}

void Sidebar::set_footer(const char* text) noexcept {
    footer_.assign(text != nullptr ? text : "");
}

void Sidebar::set_alpha(float alpha) noexcept {
    alpha_ = util::clamp01(alpha);
}

float Sidebar::row_top(const Rect& area, std::size_t index) noexcept {
    float y = area.top() + kLogoBlockHeight + kSectionLabelHeight;
    const std::size_t last = index < kSectionCount ? index : kSectionCount;
    for (std::size_t row = 0; row < last; ++row) {
        if (row + 1 == kModuleCategoryCount) {
            y += kSectionLabelHeight + theme::metrics::kNavSectionGap;
        }
        y += row_stride();
    }
    return y;
}

Rect Sidebar::row_rect(const Rect& area, std::size_t index) noexcept {
    return Rect{area.left() + kRowInset, row_top(area, index), area.w - (kRowInset * 2.0f),
        theme::metrics::kNavRowHeight};
}

const char* Sidebar::heading_for(std::size_t index) noexcept {
    if (index == 0) {
        return "MODULES";
    }
    if (index == kModuleCategoryCount) {
        return "GENERAL";
    }
    return nullptr;
}

bool Sidebar::handle_input(const Input& input) noexcept {
    if (area_.empty()) {
        return false;
    }
    const bool clicked = input.is_pressed(0);
    bool consumed = false;

    for (std::size_t index = 0; index < kSectionCount; ++index) {
        const Section section = static_cast<Section>(index);
        const bool hovered = row_rect(area_, index).contains(input.mouse_x, input.mouse_y);
        if (controller_ != nullptr) {
            controller_->set_target(hover_[index], hovered ? 1.0f : 0.0f);
        }
        if (hovered && clicked) {
            consumed = true;
            if (selected_ != section) {
                selected_ = section;
                changed_ = true;
            }
        }
    }
    return consumed;
}

void Sidebar::animate(float delta_seconds) noexcept {
    // Targets were steered in handle_input (the row under the pointer is a hit-test fact, not a
    // clock fact); nothing to advance here beyond the contract. The GUI owns the single tick.
    (void)delta_seconds;
}

void Sidebar::render(ImDrawList* draw_list, const Rect& area) noexcept {
    if (draw_list == nullptr || area.empty() || alpha_ <= 0.001f) {
        return;
    }

    draw::rounded_rect(draw_list, area, util::with_alpha(theme::color::kSidebar, alpha_),
        theme::metrics::kWindowRounding, ImDrawFlags_RoundCornersBottomLeft);

    // ── Logo block: client name over build identity ──
    const float logo_left = area.left() + kRowLabelInset;
    draw::text(draw_list, logo_left, area.top() + kLogoTop, version::kClientName,
        util::with_alpha(theme::color::kText, alpha_), theme::metrics::kTitleFontSize);
    draw::text(draw_list, logo_left,
        area.top() + kLogoTop + theme::metrics::kTitleFontSize + kLogoLabelGap, version::kVersion,
        util::with_alpha(theme::color::kTextDim, alpha_), theme::metrics::kFooterFontSize);

    // ── Rows ──
    for (std::size_t index = 0; index < kSectionCount; ++index) {
        if (const char* heading = heading_for(index); heading != nullptr) {
            draw::text(draw_list, area.left() + kRowLabelInset,
                row_top(area, index) - kSectionLabelHeight + 4.0f, heading,
                util::with_alpha(theme::color::kTextDim, alpha_),
                theme::metrics::kFooterFontSize);
        }

        const Rect row = row_rect(area, index);
        const Section section = static_cast<Section>(index);
        const float hover = hover_amount(index);
        const bool selected = (section == selected_);

        if (selected) {
            draw::rounded_rect(draw_list, row, util::with_alpha(theme::color::kNavActive, alpha_),
                theme::metrics::kFrameRounding);
            draw::rounded_rect(draw_list,
                Rect{row.left(), row.top() + 5.0f, kAccentWidth, row.h - 10.0f},
                util::with_alpha(theme::category_accent(section), alpha_), kAccentWidth * 0.5f);
        } else if (hover > 0.01f) {
            const Rgba hover_fill =
                util::mix(theme::color::kCard, theme::color::kCardHover, hover);
            draw::rounded_rect(draw_list, row, util::with_alpha(hover_fill, alpha_ * 0.7f),
                theme::metrics::kFrameRounding);
        }

        const Rgba label_color = selected
            ? theme::color::kText
            : util::mix(theme::color::kTextMuted, theme::color::kText, hover);

        // Counter badge first, so the label can be clipped to what is left of the row instead of
        // being drawn underneath the badge.
        float label_right = row.right() - kRowLabelInset;
        if (total_[index] > 0) {
            const Rect badge{row.right() - kRowInset - kBadgeWidth,
                row.center_y() - (kBadgeHeight * 0.5f), kBadgeWidth, kBadgeHeight};
            // A live category gets an accent chip; an empty one stays muted, so "0/4" reads as
            // "nothing enabled" at a glance rather than as a highlight.
            const bool live = enabled_[index] > 0;
            const Rgba badge_fill = live ? theme::color::kAccent : theme::color::kCardBorder;
            draw::rounded_rect(draw_list, badge,
                util::with_alpha(badge_fill, live ? alpha_ * 0.9f : alpha_ * 0.5f), 4.0f);

            char counts[16];
            (void)std::snprintf(counts, sizeof(counts), "%zu/%zu", enabled_[index], total_[index]);
            draw::text_in(draw_list, badge, counts,
                util::with_alpha(
                    live ? theme::color::kText : theme::color::kTextMuted, alpha_),
                draw::Align::Center, theme::metrics::kFooterFontSize);
            label_right = badge.left() - kBadgeGap;
        }

        const float label_width = label_right - (row.left() + kRowLabelInset);
        if (label_width > 4.0f) {
            draw::text_clipped(draw_list,
                Rect{row.left() + kRowLabelInset, row.top(), label_width, row.h},
                section_label(section), util::with_alpha(label_color, alpha_), draw::Align::Left);
        }
    }

    // ── Footer: build identity plus frame rate, fed in by the GUI ──
    if (!footer_.empty()) {
        draw::text_clipped(draw_list,
            Rect{area.left() + kRowLabelInset, area.bottom() - kFooterHeight,
                area.w - (kRowLabelInset * 2.0f), kFooterHeight},
            footer_.c_str(), util::with_alpha(theme::color::kTextDim, alpha_), draw::Align::Left,
            theme::metrics::kFooterFontSize);
    }
}

bool Sidebar::take_selection_changed() noexcept {
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

float Sidebar::hover_amount(std::size_t index) const noexcept {
    if (controller_ == nullptr || index >= kSectionCount) {
        return 0.0f;
    }
    return util::clamp01(controller_->value(hover_[index]));
}

} // namespace woke::ui::components
