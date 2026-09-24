#pragma once

// Easing curves (blueprint §7.4).
//
// One header for every curve the client uses, so "easeOutCubic for toggles and chevrons,
// easeOutExpo for the window scale" is a fact about one file rather than a convention spread over
// a dozen call sites. Pure functions of `t`, so they are shared by the GUI, the HUD, the toast
// pool and the host tests.
//
// `t` is treated as 0..1 and clamped, with both endpoints exact: a curve that returns 0.999 at
// t = 1 leaves a widget permanently one pixel off its target, which is exactly the kind of bug
// that only ever shows up as "it never quite settles".

#include <cmath>

#include "utils/math_utils.h"

namespace woke::ui::animation {

// Standard UI curve: fast start, gentle arrival. Toggles, chevrons, hover brightness.
[[nodiscard]] inline constexpr float ease_out_cubic(float t) noexcept {
    const float u = 1.0f - util::clamp01(t);
    return 1.0f - (u * u * u);
}

// Slower arrival than cubic; used where a longer settle reads as more deliberate.
[[nodiscard]] inline constexpr float ease_out_quint(float t) noexcept {
    const float u = 1.0f - util::clamp01(t);
    return 1.0f - (u * u * u * u * u);
}

[[nodiscard]] inline constexpr float ease_in_cubic(float t) noexcept {
    const float k = util::clamp01(t);
    return k * k * k;
}

// Symmetric curve; written as arithmetic rather than std::pow so it stays a constant expression.
[[nodiscard]] inline constexpr float ease_in_out_cubic(float t) noexcept {
    const float k = util::clamp01(t);
    if (k < 0.5f) {
        return 4.0f * k * k * k;
    }
    const float u = -2.0f * k + 2.0f;
    return 1.0f - ((u * u * u) * 0.5f);
}

[[nodiscard]] inline constexpr float ease_out_back(float t, float overshoot = 1.70158f) noexcept {
    const float u = util::clamp01(t) - 1.0f;
    return 1.0f + ((overshoot + 1.0f) * u * u * u) + (overshoot * u * u);
}

// The window scale ramp (§7.4). 2^-10t is not expressible in a constant expression with C++20's
// standard library (std::exp2 is not constexpr), so this one curve is a runtime inline function.
// The endpoint is normalised by 1 - 2^-10 so that t = 1 lands exactly on 1.0.
[[nodiscard]] inline float ease_out_expo(float t) noexcept {
    if (t <= 0.0f) {
        return 0.0f;
    }
    if (t >= 1.0f) {
        return 1.0f;
    }
    constexpr float kEndpoints = 1.0f - 0.0009765625f; // 1 - 2^-10
    return (1.0f - std::exp2(-10.0f * t)) / kEndpoints;
}

} // namespace woke::ui::animation
