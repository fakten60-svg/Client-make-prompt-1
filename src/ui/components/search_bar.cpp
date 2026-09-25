#include "ui/components/search_bar.h"

#include <cstring>

#include "ui/theme.h"
#include "utils/math_utils.h"

namespace woke::ui::components {
namespace {

using util::Rgba;

constexpr float kRounding = 8.0f;
constexpr float kTextInset = 12.0f;
constexpr float kGlyphRadius = 4.5f;
constexpr float kGlyphInset = 14.0f;

[[nodiscard]] char lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Case-insensitive containment, byte-wise and locale-free: the filter must behave identically in
// every Windows locale the game can be launched under, and only ASCII module names exist.
[[nodiscard]] bool contains_ignore_case(const char* haystack, const char* needle) noexcept {
    if (needle[0] == '\0') {
        return true;
    }
    for (const char* start = haystack; *start != '\0'; ++start) {
        const char* h = start;
        const char* n = needle;
        while (*h != '\0' && *n != '\0' && lower_ascii(*h) == lower_ascii(*n)) {
            ++h;
            ++n;
        }
        if (*n == '\0') {
            return true;
        }
    }
    return false;
}

} // namespace

void SearchBar::bind(animation::AnimationController& controller) noexcept {
    controller_ = &controller;
    focus_ = controller.acquire_state(0.0f, 16.0f);
    hover_ = controller.acquire_state(0.0f, 16.0f);
}

void SearchBar::unbind() noexcept {
    if (controller_ == nullptr) {
        return;
    }
    controller_->release(focus_);
    controller_->release(hover_);
    focus_ = animation::StateHandle{};
    hover_ = animation::StateHandle{};
    controller_ = nullptr;
}

void SearchBar::set_area(const Rect& area) noexcept {
    area_ = area;
}

void SearchBar::set_focused(bool focused) noexcept {
    focused_ = focused;
}

void SearchBar::set_alpha(float alpha) noexcept {
    alpha_ = util::clamp01(alpha);
}

bool SearchBar::push_char(unsigned int codepoint) noexcept {
    // ASCII printable only: the fixed buffer has no UTF-8 decoder, and module names are ASCII. A
    // control character (backspace is handled separately) must not end up in the query.
    if (codepoint < 0x20u || codepoint > 0x7Eu) {
        return false;
    }
    if (buffer_.size() >= kCapacity - 1) {
        return false;
    }
    char single[2] = {static_cast<char>(codepoint), '\0'};
    buffer_.append(std::string_view(single, 1));
    changed_ = true;
    return true;
}

bool SearchBar::backspace() noexcept {
    const std::size_t length = buffer_.size();
    if (length == 0) {
        return false;
    }
    // FixedString has no pop: rebuild from the truncated view, which is a memcpy of at most 64
    // bytes into the same buffer, so nothing allocates.
    const std::string_view shorter(buffer_.c_str(), length - 1);
    buffer_.assign(shorter);
    changed_ = true;
    return true;
}

void SearchBar::clear() noexcept {
    if (buffer_.empty()) {
        return;
    }
    buffer_.clear();
    changed_ = true;
}

void SearchBar::set_text(const char* text) noexcept {
    const char* source = text != nullptr ? text : "";
    const std::string_view requested(source, std::strlen(source));
    if (requested == buffer_.view()) {
        return;
    }
    buffer_.assign(requested);
    changed_ = true;
}

bool SearchBar::matches(const char* haystack) const noexcept {
    if (haystack == nullptr) {
        return false;
    }
    return contains_ignore_case(haystack, buffer_.c_str());
}

bool SearchBar::handle_input(const Input& input) noexcept {
    if (area_.empty()) {
        return false;
    }
    hovered_ = area_.contains(input.mouse_x, input.mouse_y);
    if (!input.is_pressed(0)) {
        return false;
    }
    if (hovered_) {
        focused_ = true;
        return true; // the field owns the click so the content pane cannot also act on it
    }
    // A click anywhere else drops focus, but does not consume: the click belongs to whatever it
    // landed on.
    focused_ = false;
    return false;
}

void SearchBar::animate(float delta_seconds) noexcept {
    if (controller_ == nullptr) {
        return;
    }
    (void)delta_seconds;
    controller_->set_target(focus_, focused_ ? 1.0f : 0.0f);
    controller_->set_target(hover_, hovered_ ? 1.0f : 0.0f);
}

void SearchBar::render(ImDrawList* draw_list, const Rect& area) noexcept {
    if (draw_list == nullptr || area.empty() || alpha_ <= 0.001f) {
        return;
    }

    const float focus = focus_amount();
    const float hover = controller_ != nullptr ? util::clamp01(controller_->value(hover_)) : 0.0f;
    const Rgba fill = util::mix(theme::color::kCard, theme::color::kCardHover, hover * 0.6f);
    draw::rounded_rect(draw_list, area, util::with_alpha(fill, alpha_), kRounding);
    draw::border_stroke(draw_list, area,
        util::with_alpha(util::mix(theme::color::kCardBorder, theme::color::kAccent, focus), alpha_),
        kRounding);

    // A magnifier drawn from the same primitives as everything else (circle + short handle),
    // rather than an icon font entry the overlay would have to load.
    const float glyph_x = area.left() + kGlyphInset;
    const float glyph_y = area.center_y() - 1.0f;
    const Rgba glyph_color =
        util::mix(theme::color::kTextDim, theme::color::kTextMuted, focus);
    draw::ring(draw_list, glyph_x, glyph_y, kGlyphRadius,
        util::with_alpha(glyph_color, alpha_), 1.4f);
    draw::line(draw_list, glyph_x + kGlyphRadius * 0.75f, glyph_y + kGlyphRadius * 0.75f,
        glyph_x + kGlyphRadius * 1.7f, glyph_y + kGlyphRadius * 1.7f,
        util::with_alpha(glyph_color, alpha_), 1.4f);

    const Rect text_area = area.inset(kTextInset).without_left(kGlyphInset + 4.0f);
    if (buffer_.empty()) {
        draw::text_clipped(draw_list, text_area, "Search modules...",
            util::with_alpha(theme::color::kTextDim, alpha_), draw::Align::Left);
        return;
    }
    draw::text_clipped(draw_list, text_area, buffer_.c_str(),
        util::with_alpha(theme::color::kText, alpha_), draw::Align::Left);

    // A steady caret at the end of the query while the field is focused: the game's captured
    // cursor means there is no OS caret to rely on.
    if (focus > 0.5f) {
        const float caret_x =
            text_area.left() + draw::text_width(buffer_.c_str()) + 2.0f;
        if (caret_x < text_area.right()) {
            draw::line(draw_list, caret_x, text_area.top() + 3.0f, caret_x,
                text_area.bottom() - 3.0f, util::with_alpha(theme::color::kAccent, alpha_), 1.5f);
        }
    }
}

bool SearchBar::take_changed() noexcept {
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

float SearchBar::focus_amount() const noexcept {
    return controller_ != nullptr ? util::clamp01(controller_->value(focus_)) : 0.0f;
}

} // namespace woke::ui::components
