#include "ui/notifications.h"

#include "ui/animation/easing.h"
#include "ui/theme.h"
#include "utils/math_utils.h"

namespace woke::ui {
namespace {

using draw::Align;
using draw::Rect;
using util::Rgba;

constexpr float kRounding = 10.0f;
constexpr float kPadding = 12.0f;
constexpr float kAccentStrip = 3.0f;
constexpr float kTitleHeight = 16.0f;
constexpr float kRevealSpeed = 1.0f / 0.11f; // ~110 ms to slide in or out
constexpr float kSettled = 0.02f;
// A toast is only recycled once it has been off screen for a moment: without this grace period an
// expired toast could be recycled in the same tick it started sliding out.
constexpr float kRecycleGraceSeconds = 0.20f;

[[nodiscard]] Rgba accent_for(ToastIcon icon) noexcept {
    switch (icon) {
    case ToastIcon::Success:
        return theme::color::kTrafficGreen;
    case ToastIcon::Warning:
        return theme::color::kTrafficYellow;
    default:
        return theme::color::kAccent;
    }
}

} // namespace

void Notifications::bind(animation::AnimationController& controller) noexcept {
    controller_ = &controller;
}

void Notifications::unbind() noexcept {
    clear();
    controller_ = nullptr;
}

void Notifications::set_alpha(float alpha) noexcept {
    alpha_ = util::clamp01(alpha);
}

void Notifications::set_screen(const Rect& screen) noexcept {
    screen_ = screen;
}

bool Notifications::push(const Toast& toast) noexcept {
    for (Slot& slot : slots_) {
        if (slot.active) {
            continue;
        }
        slot.title.assign(toast.title != nullptr ? toast.title : "");
        slot.message.assign(toast.message != nullptr ? toast.message : "");
        slot.icon = toast.icon;
        slot.life_seconds = toast.life_seconds > 0.05f ? toast.life_seconds : kDefaultLifeSeconds;
        slot.elapsed_seconds = 0.0f;
        slot.active = true;
        if (controller_ != nullptr && !slot.reveal.valid()) {
            slot.reveal = controller_->acquire_state(0.0f, kRevealSpeed);
        }
        if (controller_ != nullptr) {
            // Start from zero so the toast slides in from the right rather than appearing.
            controller_->set_value(slot.reveal, 0.0f);
            controller_->set_target(slot.reveal, 1.0f);
        }
        return true;
    }
    ++dropped_;
    return false;
}

bool Notifications::push(const char* title, const char* message, float life_seconds) noexcept {
    Toast toast;
    toast.title = title;
    toast.message = message;
    toast.icon = ToastIcon::Info;
    toast.life_seconds = life_seconds;
    return push(toast);
}

void Notifications::release_slot(Slot& slot) noexcept {
    if (controller_ != nullptr && slot.reveal.valid()) {
        controller_->release(slot.reveal);
    }
    slot.reveal = animation::StateHandle{};
    slot.active = false;
    slot.elapsed_seconds = 0.0f;
    slot.title.clear();
    slot.message.clear();
}

void Notifications::clear() noexcept {
    for (Slot& slot : slots_) {
        if (slot.active) {
            release_slot(slot);
        }
    }
}

void Notifications::animate(float delta_seconds) noexcept {
    if (delta_seconds < 0.0f) {
        delta_seconds = 0.0f;
    }
    for (Slot& slot : slots_) {
        if (!slot.active) {
            continue;
        }
        slot.elapsed_seconds += delta_seconds;
        const bool alive = slot.elapsed_seconds < slot.life_seconds;

        if (controller_ == nullptr) {
            // Unbound (a headless or early-boot frame): the toast still expires, it just cannot
            // animate its exit, so it is retired as soon as its life is over.
            if (!alive) {
                release_slot(slot);
            }
            continue;
        }

        controller_->set_target(slot.reveal, alive ? 1.0f : 0.0f);
        if (!alive && slot.elapsed_seconds > slot.life_seconds + kRecycleGraceSeconds
            && controller_->value(slot.reveal) <= kSettled) {
            release_slot(slot);
        }
    }
}

Rect Notifications::toast_rect(const Rect& screen, std::size_t index) const noexcept {
    if (index >= kCapacity || !slots_[index].active) {
        return Rect{};
    }
    // Visual rank = how many older slots are still on screen. This is what makes the stack close up
    // as toasts expire, instead of leaving a hole where a spent slot used to be.
    std::size_t rank = 0;
    for (std::size_t older = 0; older < index; ++older) {
        if (slots_[older].active) {
            ++rank;
        }
    }
    const float reveal = reveal_amount(index);
    const float x = screen.right() - kMargin - kWidth
        + ((1.0f - animation::ease_out_cubic(reveal)) * kSlideDistance);
    const float y = screen.top() + kMargin + (static_cast<float>(rank) * (kHeight + kSpacing));
    return Rect{x, y, kWidth, kHeight};
}

bool Notifications::handle_input(const components::Input& input) noexcept {
    if (!input.is_pressed(0) || screen_.empty()) {
        return false;
    }
    for (std::size_t index = 0; index < kCapacity; ++index) {
        if (!slots_[index].active) {
            continue;
        }
        const Rect rect = toast_rect(screen_, index);
        if (rect.contains(input.mouse_x, input.mouse_y)) {
            // Clicking a toast dismisses it: the user has read it, and a stuck toast is the most
            // common complaint about these overlays.
            slots_[index].elapsed_seconds = slots_[index].life_seconds;
            return true;
        }
    }
    return false;
}

void Notifications::render(ImDrawList* draw_list, const Rect& screen) noexcept {
    if (draw_list == nullptr || screen.empty() || alpha_ <= 0.001f) {
        return;
    }

    for (std::size_t index = 0; index < kCapacity; ++index) {
        const Slot& slot = slots_[index];
        if (!slot.active) {
            continue;
        }
        const Rect rect = toast_rect(screen, index);
        if (rect.empty()) {
            continue;
        }
        const float reveal = animation::ease_out_cubic(reveal_amount(index));
        if (reveal <= 0.002f) {
            continue;
        }
        const float opacity = alpha_ * reveal;

        draw::shadow_rect(draw_list, rect, 10.0f,
            util::scale_alpha(theme::color::kShadow, opacity));
        draw::rounded_rect(draw_list, rect, util::with_alpha(theme::color::kChromeBar, opacity),
            kRounding);
        draw::border_stroke(draw_list, rect, util::with_alpha(theme::color::kCardBorder, opacity),
            kRounding);

        // Accent strip down the left edge: the "icon", without a font atlas entry.
        const Rect strip{rect.left(), rect.top() + 8.0f, kAccentStrip, rect.h - 16.0f};
        draw::rounded_rect(draw_list, strip,
            util::with_alpha(accent_for(slot.icon), opacity), kAccentStrip * 0.5f);

        const float text_left = rect.left() + kPadding + kAccentStrip;
        const float text_width = rect.right() - kPadding - text_left;
        draw::text_clipped(draw_list,
            Rect{text_left, rect.top() + 10.0f, text_width, kTitleHeight}, slot.title.c_str(),
            util::with_alpha(theme::color::kText, opacity), Align::Left);
        draw::text_clipped(draw_list,
            Rect{text_left, rect.top() + 10.0f + kTitleHeight, text_width,
                rect.h - (10.0f + kTitleHeight) - (kProgressHeight + 6.0f)},
            slot.message.c_str(), util::with_alpha(theme::color::kTextMuted, opacity), Align::Left,
            theme::metrics::kFooterFontSize);

        // Progress bar: drains left-to-right over the toast's life, so "how long have I got" is
        // readable without a number.
        const float remaining = util::clamp01(1.0f - (slot.elapsed_seconds / slot.life_seconds));
        const Rect track{rect.left() + kPadding, rect.bottom() - kProgressHeight - 4.0f,
            rect.w - (kPadding * 2.0f), kProgressHeight};
        draw::rounded_rect(draw_list, track, util::with_alpha(theme::color::kCardBorder, opacity),
            kProgressHeight * 0.5f);
        if (remaining > 0.005f) {
            draw::rounded_rect(draw_list,
                Rect{track.left(), track.top(), track.w * remaining, track.h},
                util::with_alpha(accent_for(slot.icon), opacity), kProgressHeight * 0.5f);
        }
    }
}

bool Notifications::needs_render() const noexcept {
    for (const Slot& slot : slots_) {
        if (slot.active) {
            return true;
        }
    }
    return false;
}

std::size_t Notifications::active_count() const noexcept {
    std::size_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.active) {
            ++count;
        }
    }
    return count;
}

bool Notifications::active(std::size_t index) const noexcept {
    return index < kCapacity && slots_[index].active;
}

float Notifications::reveal_amount(std::size_t index) const noexcept {
    if (index >= kCapacity || controller_ == nullptr || !slots_[index].reveal.valid()) {
        // Unbound: report "fully shown", which is the state the toast would be in if it could not
        // animate - never a value that makes it invisible.
        return slots_[index].active ? 1.0f : 0.0f;
    }
    return util::clamp01(controller_->value(slots_[index].reveal));
}

float Notifications::elapsed(std::size_t index) const noexcept {
    return index < kCapacity ? slots_[index].elapsed_seconds : 0.0f;
}

} // namespace woke::ui
