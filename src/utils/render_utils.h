#pragma once

// ImDrawList primitives (blueprint §7.7).
//
// This is the only file allowed to touch ImDrawList directly besides ImGui's own backend, and the
// only place where the theme's floating-point colours become ImGui's packed ImU32. Everything is a
// stateless free function, so a widget composes chrome without owning state and the whole overlay
// stays visually consistent - a border is the same border everywhere because there is one
// border_stroke().
//
// No windows.h: the ImGui header is portable, so this layer (and the components on top of it) is
// compiled and exercised by the host tests on Linux, where a headless ImGui context renders the
// same draw list into memory.

#include <cstddef>

#include <imgui.h>

#include "ui/theme.h"
#include "utils/math_utils.h"

namespace woke::ui::draw {

using util::Rgba;

// Axis-aligned rectangle in screen space. Layout math happens in these, not in ImVec2, so the
// geometry is testable without ImGui.
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    [[nodiscard]] constexpr float left() const noexcept { return x; }
    [[nodiscard]] constexpr float top() const noexcept { return y; }
    [[nodiscard]] constexpr float right() const noexcept { return x + w; }
    [[nodiscard]] constexpr float bottom() const noexcept { return y + h; }
    [[nodiscard]] constexpr float center_x() const noexcept { return x + (w * 0.5f); }
    [[nodiscard]] constexpr float center_y() const noexcept { return y + (h * 0.5f); }
    [[nodiscard]] constexpr bool empty() const noexcept { return w <= 0.0f || h <= 0.0f; }

    [[nodiscard]] constexpr bool contains(float px, float py) const noexcept {
        return px >= x && px <= right() && py >= y && py <= bottom();
    }

    [[nodiscard]] constexpr Rect inset(float amount) const noexcept {
        return Rect{x + amount, y + amount, w - (amount * 2.0f), h - (amount * 2.0f)};
    }

    [[nodiscard]] constexpr Rect offset(float dx, float dy) const noexcept {
        return Rect{x + dx, y + dy, w, h};
    }

    [[nodiscard]] constexpr Rect slice_top(float height) const noexcept {
        return Rect{x, y, w, height};
    }

    [[nodiscard]] constexpr Rect slice_left(float width) const noexcept {
        return Rect{x, y, width, h};
    }

    // The strip this rect starts with, removed: `without_top(64)` of a window is its body. It is
    // the opposite of below(), which places a *new* rect after this one.
    [[nodiscard]] constexpr Rect without_top(float amount) const noexcept {
        return Rect{x, y + amount, w, h - amount};
    }

    [[nodiscard]] constexpr Rect without_left(float amount) const noexcept {
        return Rect{x + amount, y, w - amount, h};
    }

    [[nodiscard]] constexpr Rect below(float height) const noexcept {
        return Rect{x, y + h, w, height};
    }
};

// ── Colour conversion ─────────────────────────────────────────────────────────────

[[nodiscard]] ImU32 to_im_u32(const Rgba& color) noexcept;
[[nodiscard]] ImVec4 to_im_vec4(const Rgba& color) noexcept;

// ── Fills and strokes ─────────────────────────────────────────────────────────────

// `flags` is ImDrawFlags_RoundCorners*; the composition rounds only the corners a region actually
// owns (the chrome bar its top two, the sidebar its bottom-left, and so on).
void rounded_rect(ImDrawList* draw_list, const Rect& rect, const Rgba& fill, float rounding,
    ImDrawFlags flags = 0) noexcept;
void border_stroke(ImDrawList* draw_list, const Rect& rect, const Rgba& color, float rounding,
    float thickness = 1.0f) noexcept;

// Soft drop shadow in four edge quads with per-corner alpha, rather than a stack of translucent
// rectangles: one shadow costs four draw commands and never darkens itself where the segments meet
// in the middle. The quads are straight-edged - a rounded outline would cost a path and the window
// above the shadow is rounded anyway - so there is deliberately no `rounding` parameter here.
void shadow_rect(ImDrawList* draw_list, const Rect& rect, float spread,
    const Rgba& color = theme::color::kShadow) noexcept;

void gradient_fill_v(ImDrawList* draw_list, const Rect& rect, const Rgba& top,
    const Rgba& bottom) noexcept;
void gradient_fill_h(ImDrawList* draw_list, const Rect& rect, const Rgba& left,
    const Rgba& right) noexcept;

void circle(ImDrawList* draw_list, float center_x, float center_y, float radius,
    const Rgba& fill) noexcept;
void ring(ImDrawList* draw_list, float center_x, float center_y, float radius, const Rgba& color,
    float thickness = 1.0f) noexcept;
void line(ImDrawList* draw_list, float ax, float ay, float bx, float by, const Rgba& color,
    float thickness = 1.0f) noexcept;

// ── Text ──────────────────────────────────────────────────────────────────────────
//
// font_size <= 0 means "the current ImGui font size". A larger size is drawn through the
// (font, size) overload so headings never require a second font atlas entry.

[[nodiscard]] float text_width(const char* text, float font_size = 0.0f) noexcept;
[[nodiscard]] float text_height(float font_size = 0.0f) noexcept;

enum class Align { Left, Center, Right };

void text(ImDrawList* draw_list, float x, float y, const char* text, const Rgba& color,
    float font_size = 0.0f) noexcept;
void text_in(ImDrawList* draw_list, const Rect& area, const char* text, const Rgba& color,
    Align align = Align::Left, float font_size = 0.0f) noexcept;

// Draws inside `area`, ellipsizing when the text does not fit. The clipped copy lives in a
// FixedString so per-frame text can never touch the heap (§6.3).
void text_clipped(ImDrawList* draw_list, const Rect& area, const char* text, const Rgba& color,
    Align align = Align::Left, float font_size = 0.0f) noexcept;

} // namespace woke::ui::draw
