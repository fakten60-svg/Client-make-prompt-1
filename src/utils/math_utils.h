#pragma once

// Portable, header-inline math and colour helpers (blueprint §3.6, §7.2).
//
// Deliberately free of windows.h, imgui.h and any allocation: this header is what the layout,
// animation and hover code in the UI layer is written against, so the host test suite can assert
// the curves and the colour mixing on Linux instead of only seeing them in-game.
//
// Everything here is `constexpr`, which means the palette in ui/theme.h is computed at compile
// time and a wrong token is a compile error rather than a colour that looks slightly off.

#include <cstdint>

namespace woke::util {

// ── Scalars ───────────────────────────────────────────────────────────────────────

[[nodiscard]] inline constexpr float clamp(float value, float low, float high) noexcept {
    return value < low ? low : (value > high ? high : value);
}

[[nodiscard]] inline constexpr float clamp01(float value) noexcept {
    return clamp(value, 0.0f, 1.0f);
}

// Linear interpolation. `t` is not clamped: callers that need a clamped blend use clamp01 first,
// which keeps the two concerns separate and lets callers extrapolate on purpose.
[[nodiscard]] inline constexpr float lerp(float a, float b, float t) noexcept {
    return a + (b - a) * t;
}

// Where `value` sits between `low` and `high`; 0 when the range is degenerate.
[[nodiscard]] inline constexpr float inverse_lerp(float low, float high, float value) noexcept {
    return high > low ? clamp01((value - low) / (high - low)) : 0.0f;
}

// Moves `value` toward `target` by at most `max_delta`. Used where a frame-rate-independent
// exponential approach would overshoot a hard boundary (text scroll, sizes).
[[nodiscard]] inline constexpr float approach(float value, float target, float max_delta) noexcept {
    const float delta = target - value;
    if (delta > max_delta) {
        return value + max_delta;
    }
    if (delta < -max_delta) {
        return value - max_delta;
    }
    return target;
}

[[nodiscard]] inline constexpr bool nearly_equal(float a, float b, float epsilon = 0.0005f) noexcept {
    return (a > b ? a - b : b - a) <= epsilon;
}

// ── Colour ────────────────────────────────────────────────────────────────────────
//
// Straight (non-premultiplied) RGBA in 0..1. The UI layer stays in this representation and only
// converts to ImGui's packed ImU32 at the draw-call boundary (utils/render_utils.h), so colour
// math never has to unpack bytes.

struct Rgba {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;

    [[nodiscard]] friend constexpr bool operator==(const Rgba&, const Rgba&) noexcept = default;
};

[[nodiscard]] inline constexpr float channel(std::uint32_t byte_value) noexcept {
    return static_cast<float>(byte_value & 0xFFu) / 255.0f;
}

[[nodiscard]] inline constexpr Rgba rgba(float r, float g, float b, float a = 1.0f) noexcept {
    return Rgba{r, g, b, a};
}

// 0xRRGGBB -> Rgba. The palette table in ui/theme.h reads exactly like the blueprint's hex
// column because of this, so a token can be checked against §7.2 by eye.
[[nodiscard]] inline constexpr Rgba from_hex(std::uint32_t rgb, float alpha = 1.0f) noexcept {
    return Rgba{channel(rgb >> 16), channel(rgb >> 8), channel(rgb), alpha};
}

[[nodiscard]] inline constexpr Rgba with_alpha(const Rgba& color, float alpha) noexcept {
    return Rgba{color.r, color.g, color.b, clamp01(alpha)};
}

[[nodiscard]] inline constexpr Rgba scale_alpha(const Rgba& color, float factor) noexcept {
    return with_alpha(color, color.a * clamp01(factor));
}

// Colour cross-fade. Used by the pill toggle (dark gray -> Apple blue), card hover and the
// traffic-light hover ring, so all three obviously share one curve.
[[nodiscard]] inline constexpr Rgba mix(const Rgba& a, const Rgba& b, float t) noexcept {
    const float k = clamp01(t);
    return Rgba{lerp(a.r, b.r, k), lerp(a.g, b.g, k), lerp(a.b, b.b, k), lerp(a.a, b.a, k)};
}

// Brightness multiplier for hover states, clamped so a token cannot be pushed out of range.
[[nodiscard]] inline constexpr Rgba scale_rgb(const Rgba& color, float factor) noexcept {
    return Rgba{clamp01(color.r * factor), clamp01(color.g * factor), clamp01(color.b * factor),
        color.a};
}

inline constexpr Rgba kWhite = from_hex(0xFFFFFF);
inline constexpr Rgba kBlack = from_hex(0x000000);
inline constexpr Rgba kTransparent = from_hex(0x000000, 0.0f);

} // namespace woke::util
