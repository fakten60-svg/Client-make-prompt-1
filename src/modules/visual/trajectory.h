#pragma once

// Projectile path prediction and world-to-screen projection (blueprint §8: Trajectories).
//
// Pure math, header-only, and free of jni.h / windows.h / imgui.h on purpose: the host test
// suite asserts the parabola and the projection on Linux, which is the only part of a
// trajectory overlay a screenshot could not prove. Nothing here reads the game or allocates.

#include <array>
#include <cmath>
#include <cstddef>

#include "jni/game_types.h"

namespace woke::modules::visual {

using game::Vec3d;

// Three seconds at the game's 20 Hz step is 60 samples; 96 leaves headroom for a longer
// "seconds" setting without ever growing the buffer (§6.3).
inline constexpr std::size_t kMaxTrajectoryPoints = 96;

inline constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

// A point closer than this to the eye is behind the near plane; dividing by it would draw a
// mirrored smear across the screen instead of nothing.
inline constexpr double kNearPlane = 0.05;

struct TrajectoryParams {
    double gravity = 0.05;      // blocks per tick^2 (an arrow's default)
    double drag = 0.99;         // per-tick velocity retention
    double seconds = 3.0;
    int ticks_per_second = 20;
};

// Integrates the game's own discrete step rather than a closed-form parabola: the drag is
// applied to the velocity *after* it moved the point, which is the order the server-validated
// projectile actually uses.
//
// `drag` is applied to the horizontal axes separately from the vertical one because vanilla
// subtracts gravity from the retained vertical velocity, not from the raw one.
inline std::size_t predict_trajectory(const Vec3d& origin, const Vec3d& velocity,
    const TrajectoryParams& params, std::array<Vec3d, kMaxTrajectoryPoints>& out) noexcept {
    const int rate = params.ticks_per_second > 0 ? params.ticks_per_second : 20;
    const double seconds = params.seconds > 0.0 ? params.seconds : 0.0;
    const double drag = params.drag < 0.0 ? 0.0 : (params.drag > 1.0 ? 1.0 : params.drag);

    const int steps = static_cast<int>(seconds * static_cast<double>(rate));
    std::size_t count = 0;
    Vec3d position = origin;
    Vec3d step = velocity;
    for (int index = 0; index < steps && count < out.size(); ++index) {
        position.x += step.x;
        position.y += step.y;
        position.z += step.z;

        step.x *= drag;
        step.z *= drag;
        step.y = (step.y * drag) - params.gravity;

        out[count++] = position;
    }
    return count;
}

struct ScreenPoint {
    float x = 0.0f;
    float y = 0.0f;
};

// The camera a projection is taken against. FOV is vertical, matching the game's own
// field-of-view option, so a zoomed camera narrows the view rather than the aspect.
struct View {
    Vec3d eye{};
    float yaw_degrees = 0.0f;
    float pitch_degrees = 0.0f;
    float vertical_fov_degrees = 70.0f;
    float viewport_width = 1920.0f;
    float viewport_height = 1080.0f;
};

// Minecraft's convention: yaw 0 looks toward +Z and turns clockwise seen from above, and a
// positive pitch looks down.
//
// Returns false when the point is behind the near plane. The caller skips that sample, which
// is what keeps a path that passes behind the player from drawing a line across the screen.
[[nodiscard]] inline bool project_to_screen(
    const View& view, const Vec3d& point, ScreenPoint& out) noexcept {
    const double yaw = static_cast<double>(view.yaw_degrees) * kDegreesToRadians;
    const double pitch = static_cast<double>(view.pitch_degrees) * kDegreesToRadians;
    const double cos_pitch = std::cos(pitch);
    const double sin_pitch = std::sin(pitch);
    const double cos_yaw = std::cos(yaw);
    const double sin_yaw = std::sin(yaw);

    const double fx = -sin_yaw * cos_pitch;
    const double fy = -sin_pitch;
    const double fz = cos_yaw * cos_pitch;

    const double rx = cos_yaw;
    const double rz = sin_yaw;

    // up = forward x right (right x forward would point at the player's feet)
    const double ux = fy * rz;
    const double uy = fz * rx - fx * rz;
    const double uz = -fy * rx;

    const double dx = point.x - view.eye.x;
    const double dy = point.y - view.eye.y;
    const double dz = point.z - view.eye.z;

    const double camera_x = (dx * rx) + (dz * rz);
    const double camera_y = (dx * ux) + (dy * uy) + (dz * uz);
    const double camera_z = (dx * fx) + (dy * fy) + (dz * fz);
    if (camera_z <= kNearPlane) {
        return false;
    }

    const double half_fov = static_cast<double>(view.vertical_fov_degrees) * kDegreesToRadians * 0.5;
    const double focal = (static_cast<double>(view.viewport_height) * 0.5) / std::tan(half_fov);
    out.x = static_cast<float>(
        (static_cast<double>(view.viewport_width) * 0.5) + ((camera_x / camera_z) * focal));
    out.y = static_cast<float>(
        (static_cast<double>(view.viewport_height) * 0.5) - ((camera_y / camera_z) * focal));
    return true;
}

} // namespace woke::modules::visual
