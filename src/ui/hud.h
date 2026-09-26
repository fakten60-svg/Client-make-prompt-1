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
#include <cstdint>

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

// The shared accent palette (accent, white, green, red, cyan) for the step-8 modules whose
// colour setting is an enum - the Attack Cooldown bar and the Smash Flash. Index-matched to
// those modules' label tables; an out-of-range index falls back to the accent so a bad config
// value cannot draw an invisible element.
[[nodiscard]] Rgba accent_color_for(std::size_t index) noexcept;

// One frame's worth of overlay input, resolved by the caller. Plain data, so the renderer is a
// pure function of it and the host tests can construct any case directly.
struct Frame {
    Rect screen{};
    float frames_per_second = 0.0f;
    float alpha = 1.0f;

    bool watermark = false;
    bool arraylist = false;
    bool arraylist_by_length = false;

    // The speed chip (roadmap step 8: Movement/Velocity Display). `velocity_units` indexes that
    // module's unit enum; the renderer owns its own label table, so this layer never includes a
    // module header to format a number.
    bool velocity_chip = false;
    std::size_t velocity_units = 0;

    // ── Step 8: Combat and Mace readouts ────────────────────────────────────────
    //
    // Everything below follows the velocity chip's shape: the module decides *that* and *how*,
    // the frame builder supplies the live values (already read on the game thread), and the
    // renderer is a pure function of the frame. Reads gate on `world_live` like the trajectory's
    // guard, so a stale number can never outlive the world it came from - except the two
    // counter chips, which are session data that stay meaningful with no world at all.

    // Target HUD (Combat): the card about the entity under the crosshair.
    bool target_card = false;
    std::size_t target_position = 0; // 0 = under the crosshair, 1 = left chip column
    float target_scale = 1.0f;
    game::TargetInfo target{};

    // Attack Cooldown (Combat): progress 0..1 from the game's own cooldown clock.
    bool cooldown_bar = false;
    std::size_t cooldown_style = 0; // 0 = bar under the crosshair, 1 = arc around it
    Rgba cooldown_color = util::from_hex(0x0A84FF);
    float cooldown_progress = 0.0f;

    // Reach Display (Combat): the player's own entity interaction range.
    bool reach_chip = false;
    float reach_blocks = 3.0f;

    // Session counters. Drawn with or without a live world: they are the session's data.
    bool combat_counters = false;
    std::uint32_t counter_swings = 0;
    std::uint32_t counter_hits = 0;
    std::uint32_t counter_wasted = 0;

    bool mace_counters = false;
    std::uint32_t mace_swing_count = 0;
    std::uint32_t mace_hit_count = 0;
    std::uint32_t mace_smash_count = 0;

    // Smash Potential (Mace): projected damage from the player's current fall.
    bool smash_chip = false;
    double fall_distance = 0.0;
    bool smash_enhanced = false;

    // Smash Flash (Mace): the local edge flash when a smash is ready.
    bool smash_flash = false;
    float flash_intensity = 0.5f;

    // ── Step 8c: Misc / Movement / Spear readouts ─────────────────────────────
    //
    // Same shape as the step-8b chips: the module decides *that* and *how*, the frame builder
    // supplies the live values, and the renderer is a pure function of the frame. The step-8c chips
    // share the step-8b left-hand column, so any combination stacks instead of overlapping.

    // Friend Manager (Misc): a local list's size. Session data, like the counters.
    bool friend_chip = false;
    std::uint32_t friend_count = 0;

    // Safe Walk (Movement): engaged while the module holds the game's sneak key.
    bool safe_walk_chip = false;
    bool safe_walk_engaged = false;

    // Riptide Indicator (Spear): the game's own riptide flag and whether a trident is held.
    bool riptide_chip = false;
    bool riptide_engaged = false;
    bool riptide_trident = false;

    // Trident Cooldown (Spear): the trident's swing charge, shown while a trident is held.
    bool trident_chip = false;
    float trident_progress = 0.0f;
    Rgba trident_color = util::from_hex(0x3FD2E0);

    // Loyalty HUD (Spear): the client-observed throw-and-return trip.
    bool loyalty_chip = false;
    bool loyalty_tracking = false;
    double loyalty_seconds = 0.0;
    double loyalty_last_trip = 0.0;

    // The frame's own delta, so a module that observes across frames (Loyalty HUD) advances on the
    // same clock as everything else instead of assuming a rate (§7.4: one clock, one tick).
    float delta_seconds = 0.0f;

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
