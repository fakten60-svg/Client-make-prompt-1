#pragma once

// Custom crosshair geometry (blueprint §8: Custom Crosshair).
//
// Pure: three numeric settings become the handful of line segments, the optional dot and the
// optional ring a renderer draws. Keeping the arithmetic here means the renderer stays dumb and
// the shape is asserted by the host test suite instead of only by eye.
//
// Coordinates are centred on the crosshair (0,0) in screen pixels, +x right and +y down, which
// is the same space the draw list uses once the caller adds the screen centre.

#include <array>
#include <cstddef>
#include <cstdint>

namespace woke::modules::visual {

enum class CrosshairShape : std::uint8_t {
    Cross = 0,
    Dot,
    Circle,
    CrossDot,
    Count,
};

inline constexpr std::size_t kCrosshairShapeCount = static_cast<std::size_t>(CrosshairShape::Count);

[[nodiscard]] constexpr const char* crosshair_shape_label(CrosshairShape shape) noexcept {
    switch (shape) {
    case CrosshairShape::Cross:
        return "cross";
    case CrosshairShape::Dot:
        return "dot";
    case CrosshairShape::Circle:
        return "circle";
    case CrosshairShape::CrossDot:
        return "cross+dot";
    default:
        return "?";
    }
}

struct Segment {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
};

struct CrosshairGeometry {
    // Up to four arms: left, right, up, down. Only the first arm_count are meaningful.
    std::array<Segment, 4> arms{};
    std::size_t arm_count = 0;
    bool dot = false;
    float dot_radius = 1.5f;
    bool ring = false;
    float ring_radius = 6.0f;
    float thickness = 1.0f;
};

// `size` is the arm length measured from the gap outward; `gap` is the empty space left in the
// middle so the cross never covers the target pixel it is aiming with.
[[nodiscard]] inline CrosshairGeometry crosshair_geometry(
    CrosshairShape shape, float size, float gap, float thickness) noexcept {
    CrosshairGeometry geometry{};
    const float arm = size < 1.0f ? 1.0f : size;
    const float inner = gap < 0.0f ? 0.0f : gap;
    geometry.thickness = thickness < 0.5f ? 0.5f : thickness;
    geometry.dot_radius = geometry.thickness * 1.1f;
    geometry.ring_radius = inner + (arm * 0.5f);

    const bool wants_cross = shape == CrosshairShape::Cross || shape == CrosshairShape::CrossDot;
    const bool wants_dot = shape == CrosshairShape::Dot || shape == CrosshairShape::CrossDot;
    const bool wants_ring = shape == CrosshairShape::Circle;

    if (wants_cross) {
        const float outer = inner + arm;
        geometry.arms[geometry.arm_count++] = Segment{-outer, 0.0f, -inner, 0.0f};
        geometry.arms[geometry.arm_count++] = Segment{inner, 0.0f, outer, 0.0f};
        geometry.arms[geometry.arm_count++] = Segment{0.0f, -outer, 0.0f, -inner};
        geometry.arms[geometry.arm_count++] = Segment{0.0f, inner, 0.0f, outer};
    }

    geometry.dot = wants_dot;
    geometry.ring = wants_ring;
    return geometry;
}

} // namespace woke::modules::visual
