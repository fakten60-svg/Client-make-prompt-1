#include "ui/components/keybind_badge.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "ui/theme.h"
#include "utils/math_utils.h"

namespace woke::ui::components {
namespace {

using util::Rgba;

constexpr float kRounding = 4.0f;
constexpr float kHoverMix = 0.18f;
constexpr float kPulseSpeed = 1.0f / 0.11f; // ramp the ring in over ~110 ms
constexpr float kRingSpread = 2.0f;
constexpr float kTextInset = 6.0f;

// Small helper so every branch below stays a single statement.
void copy_label(char* out, std::size_t capacity, const char* label) noexcept {
    if (capacity == 0) {
        return;
    }
    const std::size_t length = std::strlen(label);
    const std::size_t take = length < (capacity - 1) ? length : (capacity - 1);
    std::memcpy(out, label, take);
    out[take] = '\0';
}

} // namespace

void format_key(int virtual_key, char* out, std::size_t capacity) noexcept {
    if (out == nullptr || capacity == 0) {
        return;
    }
    out[0] = '\0';

    if (virtual_key == 0) {
        copy_label(out, capacity, "NONE");
        return;
    }
    // Letters and digits are their own labels; this is the common case for module binds.
    if ((virtual_key >= 'A' && virtual_key <= 'Z') || (virtual_key >= '0' && virtual_key <= '9')) {
        out[0] = static_cast<char>(virtual_key);
        out[1 < capacity ? 1 : 0] = '\0';
        return;
    }
    if (virtual_key >= kKeyF1 && virtual_key <= kKeyF12) {
        (void)std::snprintf(out, capacity, "F%d", virtual_key - kKeyF1 + 1);
        return;
    }

    const char* named = nullptr;
    switch (virtual_key) {
    case kKeyBackspace:
        named = "BACKSPACE";
        break;
    case kKeyTab:
        named = "TAB";
        break;
    case kKeyEnter:
        named = "ENTER";
        break;
    case kKeyShift:
        named = "SHIFT";
        break;
    case kKeyControl:
        named = "CTRL";
        break;
    case kKeyAltMenu:
        named = "ALT";
        break;
    case kKeyCapsLock:
        named = "CAPS";
        break;
    case kKeyEscape:
        named = "ESC";
        break;
    case kKeySpace:
        named = "SPACE";
        break;
    case kKeyPageUp:
        named = "PGUP";
        break;
    case kKeyPageDown:
        named = "PGDN";
        break;
    case kKeyEnd:
        named = "END";
        break;
    case kKeyHome:
        named = "HOME";
        break;
    case kKeyLeft:
        named = "LEFT";
        break;
    case kKeyUp:
        named = "UP";
        break;
    case kKeyRight:
        named = "RIGHT";
        break;
    case kKeyDown:
        named = "DOWN";
        break;
    case kKeyInsert:
        named = "INSERT";
        break;
    case kKeyDelete:
        named = "DELETE";
        break;
    case kKeyLeftShift:
        named = "LSHIFT";
        break;
    case kKeyRightShift:
        named = "RSHIFT";
        break;
    case kKeyLeftControl:
        named = "LCTRL";
        break;
    case kKeyRightControl:
        named = "RCTRL";
        break;
    case kKeyLeftAlt:
        named = "LALT";
        break;
    case kKeyRightAlt:
        named = "RALT";
        break;
    default:
        break;
    }

    if (named != nullptr) {
        copy_label(out, capacity, named);
        return;
    }
    // Unnamed codes still get a chip label: "0xNN" is honest and, unlike a blank chip, still
    // tells the user which key is bound.
    (void)std::snprintf(out, capacity, "0x%02X", static_cast<unsigned int>(virtual_key) & 0xFFu);
}

void KeybindBadge::bind(animation::AnimationController& controller) noexcept {
    controller_ = &controller;
    hover_ = controller.acquire_state(0.0f, 16.0f);
    pulse_ = controller.acquire_state(0.0f, kPulseSpeed);
}

void KeybindBadge::unbind() noexcept {
    if (controller_ == nullptr) {
        return;
    }
    controller_->release(hover_);
    controller_->release(pulse_);
    hover_ = animation::StateHandle{};
    pulse_ = animation::StateHandle{};
    controller_ = nullptr;
}

void KeybindBadge::set_area(const Rect& area) noexcept {
    area_ = area;
}

void KeybindBadge::set_bind(int virtual_key) noexcept {
    bind_ = virtual_key < 0 ? 0 : virtual_key;
}

void KeybindBadge::set_capture_armed(bool armed) noexcept {
    if (armed_ && !armed) {
        // Cancelled or resolved: the ring must not stay lit on the next arm.
        if (controller_ != nullptr) {
            controller_->set_value(pulse_, 0.0f);
        }
    }
    armed_ = armed;
}

void KeybindBadge::set_alpha(float alpha) noexcept {
    alpha_ = util::clamp01(alpha);
}

bool KeybindBadge::accept_key(int virtual_key) noexcept {
    if (!armed_) {
        return false;
    }
    // ESC cancels: the common expectation, and the only key that must not be able to become a bind
    // without leaving the user a way out of capture mode.
    if (virtual_key != kKeyEscape && virtual_key != 0) {
        bind_ = virtual_key;
    }
    armed_ = false;
    return true;
}

bool KeybindBadge::handle_input(const Input& input) noexcept {
    hovered_ = !area_.empty() && area_.contains(input.mouse_x, input.mouse_y);
    if (hovered_ && input.is_pressed(0)) {
        clicked_ = true;
        return true; // the chip owns the click: the card underneath must not also toggle
    }
    return false;
}

void KeybindBadge::animate(float delta_seconds) noexcept {
    if (controller_ == nullptr) {
        return;
    }
    (void)delta_seconds; // targets only; the GUI owns the single controller tick
    controller_->set_target(hover_, hovered_ ? 1.0f : 0.0f);
    controller_->set_target(pulse_, armed_ ? 1.0f : 0.0f);
}

void KeybindBadge::render(ImDrawList* draw_list, const Rect& area) noexcept {
    if (draw_list == nullptr || area.empty() || alpha_ <= 0.001f) {
        return;
    }

    const float hover = hover_amount();
    const float pulse = pulse_amount();
    const Rgba fill_base =
        util::mix(theme::color::kNavActive, theme::color::kCardHover, hover * 0.5f);
    const Rgba fill = util::mix(fill_base, theme::color::kAccent, pulse * 0.85f);
    draw::rounded_rect(draw_list, area, util::with_alpha(fill, alpha_), kRounding);

    const Rgba border_base = util::mix(theme::color::kCardBorder, theme::color::kTextMuted, hover);
    const Rgba border = util::mix(border_base, theme::color::kAccent, pulse);
    draw::border_stroke(draw_list, area, util::with_alpha(border, alpha_), kRounding);

    if (pulse > 0.02f) {
        // An outward ring is the "waiting for a key" affordance; it cannot be mistaken for a
        // hover highlight because it sits outside the chip's own border.
        draw::border_stroke(draw_list, area.inset(-kRingSpread),
            util::scale_alpha(theme::color::kAccent, pulse * 0.6f), kRounding + kRingSpread);
    }

    char label[16];
    format_key(bind_, label, sizeof(label));
    const char* text = armed_ ? "..." : label;
    const Rgba text_color = armed_ ? theme::color::kText
                                   : util::mix(theme::color::kTextMuted, theme::color::kText, hover);
    draw::text_clipped(draw_list, area.inset(kTextInset), text, util::with_alpha(text_color, alpha_),
        draw::Align::Center, theme::metrics::kFooterFontSize);
}

bool KeybindBadge::take_click() noexcept {
    const bool clicked = clicked_;
    clicked_ = false;
    return clicked;
}

float KeybindBadge::hover_amount() const noexcept {
    return controller_ != nullptr ? util::clamp01(controller_->value(hover_)) : 0.0f;
}

float KeybindBadge::pulse_amount() const noexcept {
    return controller_ != nullptr ? util::clamp01(controller_->value(pulse_)) : 0.0f;
}

} // namespace woke::ui::components
