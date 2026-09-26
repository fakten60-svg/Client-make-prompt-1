#include "ui/gui_internal.h"

#include <windows.h>

#include <cstddef>

#include <imgui.h>

#include "core/build_config.h"
#include "hooks/game_thread.h"
#include "modules/module_manager.h"
#include "modules/movement/velocity_display.h"
#include "modules/visual/custom_crosshair.h"
#include "modules/visual/hud_module.h"
#include "modules/visual/trajectories.h"
#include "ui/hud.h"
#include "ui/overlay.h"
#include "ui/step8_frame.h"

#if WOKE_HAVE_JNI
#include "jni/game_instance.h"
#endif

// The in-world overlay (blueprint §7.6, roadmap step 7).
//
// The HUD/crosshair/trajectory layer that draws above the game whenever an overlay module is
// enabled, whether or not the ClickGUI chrome is visible. gui_overlay.cpp owns the per-frame
// path and calls render_inworld_overlay() from the tick; gui_chrome.cpp consults
// inworld_overlay_wanted() when it computes the frame scheduler's awake flag. Both reach this
// file through gui_internal.h, so the overlay policy is still overlay.cpp's - portable,
// host-tested, one predicate.

namespace woke::ui {
namespace detail {
namespace {

using modules::movement::VelocityDisplay;
using modules::visual::CustomCrosshair;
using modules::visual::HudModule;
using modules::visual::Trajectories;

// The in-world overlay modules, resolved by name and re-resolved only when the registry size
// changes. Cached because the draw path asks about them every frame, and a name lookup per frame
// would be thirty-odd string compares for a fact that changes at most once per click.
struct OverlayModules {
    const HudModule* hud = nullptr;
    const CustomCrosshair* crosshair = nullptr;
    const Trajectories* trajectories = nullptr;
    const VelocityDisplay* velocity = nullptr;
    std::size_t resolved_count = static_cast<std::size_t>(-1);
};

OverlayModules g_overlay{};

void resolve_overlay_modules() noexcept {
    const std::size_t count = modules::manager().count();
    if (count == g_overlay.resolved_count) {
        return;
    }
    g_overlay.resolved_count = count;
    // dynamic_cast, not static_cast: the lookup is by name, so the cast is only sound while that
    // name still maps to the type it claims to. A future name collision becomes a null pointer - a
    // silent overlay that does not draw - instead of undefined behaviour.
    g_overlay.hud = dynamic_cast<const HudModule*>(modules::manager().find("HUD"));
    g_overlay.crosshair =
        dynamic_cast<const CustomCrosshair*>(modules::manager().find("Custom Crosshair"));
    g_overlay.trajectories =
        dynamic_cast<const Trajectories*>(modules::manager().find("Trajectories"));
    g_overlay.velocity =
        dynamic_cast<const VelocityDisplay*>(modules::manager().find("Velocity Display"));
}

// The live player view for the trajectory prediction. Read only when a path is actually going to
// be drawn: four JNI calls per frame for a hidden overlay would be pure waste.
void read_player_view(hud::Frame& frame) noexcept {
#if WOKE_HAVE_JNI
    const auto eye = game::player_eye_position();
    const auto yaw = game::player_yaw();
    const auto pitch = game::player_pitch();
    const auto velocity = game::player_velocity();
    if (!eye.valid || !yaw.valid || !pitch.valid) {
        return;
    }
    frame.world_live = true;
    frame.eye = eye.value;
    frame.yaw_degrees = yaw.value;
    frame.pitch_degrees = pitch.value;
    if (velocity.valid) {
        frame.velocity = velocity.value;
    }
#else
    (void)frame;
#endif
}

void build_hud_frame(hud::Frame& frame) noexcept {
    resolve_overlay_modules();
    const ImGuiIO& io = ImGui::GetIO();
    frame.screen = Rect{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y};
    frame.frames_per_second = hooks::game_thread::stats().frames_per_second;
    frame.alpha = 1.0f;
    // The frame's own delta: the Loyalty HUD's throw timer advances on this, so it follows the real
    // frame rate instead of assuming one (§7.4).
    frame.delta_seconds = io.DeltaTime;

    if (g_overlay.hud != nullptr && g_overlay.hud->enabled()) {
        frame.watermark = g_overlay.hud->watermark();
        frame.arraylist = g_overlay.hud->arraylist();
        frame.arraylist_by_length = g_overlay.hud->sort_by_length();
    }

    if (g_overlay.crosshair != nullptr && g_overlay.crosshair->enabled()) {
        frame.crosshair = true;
        frame.crosshair_style.shape = g_overlay.crosshair->shape();
        frame.crosshair_style.size = g_overlay.crosshair->size();
        frame.crosshair_style.gap = g_overlay.crosshair->gap();
        frame.crosshair_style.thickness = g_overlay.crosshair->thickness();
        frame.crosshair_style.color =
            CustomCrosshair::color_for(g_overlay.crosshair->color_index());
    }

    if (g_overlay.velocity != nullptr && g_overlay.velocity->enabled()) {
        frame.velocity_chip = true;
        frame.velocity_units = g_overlay.velocity->units_index();
        read_player_view(frame);
    }

    if (g_overlay.trajectories != nullptr && g_overlay.trajectories->enabled()) {
        frame.trajectory = true;
        frame.trajectory_style.params = g_overlay.trajectories->params();
        frame.trajectory_style.color =
            Trajectories::color_for(g_overlay.trajectories->color_index());
        read_player_view(frame);
    }

    // Step 8: the Combat/Mace readouts. Filled last so the modules that share the left-hand chip
    // column (reach, smash, counters) read the same frame the step-7 fields already populated.
    fill_step8_readouts(frame);
}

} // namespace

bool inworld_overlay_wanted() noexcept {
    return overlay::wanted();
}

// Drawn after the chrome, so the HUD, crosshair and path sit above the game. It yields while the
// ClickGUI is open: the chrome is the focus then, and a crosshair drawn at its centre would
// otherwise land on top of the module cards.
void render_inworld_overlay() noexcept {
    const State& s = state();
    if (util::clamp01(s.animation.value(s.open_spring)) > 0.001f) {
        return;
    }
    hud::Frame frame{};
    build_hud_frame(frame);
    if (!hud::active(frame)) {
        return;
    }
    hud::render(ImGui::GetForegroundDrawList(), frame);
}

} // namespace detail
} // namespace woke::ui
