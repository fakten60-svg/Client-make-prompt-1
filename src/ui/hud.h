#pragma once

// In-world overlay (blueprint §7.6) - roadmap step 7: the HUD, the custom crosshair and the
// trajectory prediction.
//
// Portable on purpose: this layer needs only an ImDrawList, the module registry and the pure
// visual helpers, so the host test suite renders it headlessly and asserts what it emits. The
// Windows composition (gui.cpp) is what fills a Frame from the game façade; nothing in this file
// reads the game, and nothing here allocates.
//
// All geometry goes through utils/render_utils, which is the only layer permitted to touch
// ImDrawList primitives (§7.7) - this file respects that, so a re-theme stays one file.

#include <array>
#include <cstddef>

#include <imgui.h>

#include "jni/game_types.h"
#include "modules/visual/crosshair_geometry.h"
#include "modules/visual/trajectory.h"
#include "utils/math_utils.h"
#include "utils/render_utils.h"

namespace woke::ui::hud {

// The visual helpers the frame carries: the renderer draws what the modules configured, and these
// are the shared vocabulary (a shape enum, a projection routine) rather than module internals.
namespace visual = woke::modules::visual;

using draw::Rect;
using util::Rgba;

// The crosshair's resolved style. The module owns which values these are; the renderer only
// draws what it is handed.
struct CrosshairStyle {
    visual::CrosshairShape shape = visual::CrosshairShape::Cross;
    float size = 8.0f;
    float gap = 3.0f;
    float thickness = 1.5f;
    Rgba color = util::from_hex(0xFFFFFF);
    bool outline = true;
};

struct TrajectoryStyle {
    visual::TrajectoryParams params{};
    Rgba color = util::from_hex(0x0A84FF);
};

// One frame's worth of overlay input, resolved by the caller. Plain data, so the renderer is a
// pure function of it and the host tests can construct any case directly.
struct Frame {
    Rect screen{};
    float frames_per_second = 0.0f;
    float alpha = 1.0f;

    bool watermark = false;
    bool arraylist = false;
    bool arraylist_by_length = false;

    bool crosshair = false;
    CrosshairStyle crosshair_style{};

    bool trajectory = false;
    TrajectoryStyle trajectory_style{};

    // Live player state for the prediction. Only consulted when trajectory is set; `world_live`
    // is the caller's "the reads succeeded" flag, so the renderer never draws a stale path.
    bool world_live = false;
    game::Vec3d eye{};
    game::Vec3d velocity{};
    float yaw_degrees = 0.0f;
    float pitch_degrees = 0.0f;
    float vertical_fov_degrees = 70.0f;
};

// True when this frame would emit anything. The caller folds it into the §7.8 suppression
// predicate, which is what lets an enabled HUD keep the frame pipeline alive with the chrome
// hidden.
[[nodiscard]] bool active(const Frame& frame) noexcept;

void render(ImDrawList* draw_list, const Frame& frame) noexcept;

// The projected path, exposed so the host tests can assert the projection without a draw list.
// Returns how many screen points were produced; points behind the near plane are skipped rather
// than written, so the count is the number of usable samples.
[[nodiscard]] std::size_t project_trajectory(
    const Frame& frame, std::array<visual::ScreenPoint, visual::kMaxTrajectoryPoints>& out) noexcept;

} // namespace woke::ui::hud
