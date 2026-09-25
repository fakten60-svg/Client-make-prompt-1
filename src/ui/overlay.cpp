#include "ui/overlay.h"

#include "modules/module_manager.h"
#include "modules/movement/velocity_display.h"
#include "modules/visual/custom_crosshair.h"
#include "modules/visual/hud_module.h"
#include "modules/visual/trajectories.h"

namespace woke::ui::overlay {
namespace {

// The names are the registration names in lifecycle.cpp, which is the only place a module object
// is created - so these lookups are exact, and the cast below is sound.
constexpr const char* kHudName = "HUD";
constexpr const char* kCrosshairName = "Custom Crosshair";
constexpr const char* kTrajectoriesName = "Trajectories";
constexpr const char* kVelocityName = "Velocity Display";

} // namespace

bool wanted() noexcept {
    const modules::ModuleManager& registry = modules::manager();

    // dynamic_cast, not static_cast: a lookup by name is only sound while that name still maps to
    // the type it claims to. A future name collision degrades to "the overlay does not draw"
    // instead of undefined behaviour.
    if (const auto* hud = dynamic_cast<const modules::visual::HudModule*>(registry.find(kHudName));
        hud != nullptr && hud->draws()) {
        return true;
    }
    if (const auto* crosshair =
            dynamic_cast<const modules::visual::CustomCrosshair*>(registry.find(kCrosshairName));
        crosshair != nullptr && crosshair->enabled()) {
        return true;
    }
    if (const auto* velocity =
            dynamic_cast<const modules::movement::VelocityDisplay*>(registry.find(kVelocityName));
        velocity != nullptr && velocity->enabled()) {
        return true;
    }
    const auto* trajectories =
        dynamic_cast<const modules::visual::Trajectories*>(registry.find(kTrajectoriesName));
    return trajectories != nullptr && trajectories->enabled();
}

} // namespace woke::ui::overlay
