#include "ui/overlay.h"

#include "modules/combat/attack_cooldown.h"
#include "modules/combat/combat_stats.h"
#include "modules/combat/reach_display.h"
#include "modules/combat/target_hud.h"
#include "modules/mace/mace_stats.h"
#include "modules/mace/smash_flash.h"
#include "modules/mace/smash_potential.h"
#include "modules/mace/wind_charge_cd.h"
#include "modules/misc/friend_manager.h"
#include "modules/module_manager.h"
#include "modules/movement/safe_walk.h"
#include "modules/movement/velocity_display.h"
#include "modules/spear/loyalty_hud.h"
#include "modules/spear/riptide_indicator.h"
#include "modules/spear/trident_cooldown.h"
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

// Step 8 (Combat / Mace). The counter modules are in this list on purpose: their chips are session
// data that draw with no world loaded, so an enabled counter keeps the frame pipeline alive on its
// own, exactly like the watermark.
constexpr const char* kTargetHudName = "Target HUD";
constexpr const char* kCooldownName = "Attack Cooldown";
constexpr const char* kReachName = "Reach Display";
constexpr const char* kCombatStatsName = "Combat Stats";
constexpr const char* kSmashName = "Smash Potential";
constexpr const char* kSmashFlashName = "Smash Flash";
constexpr const char* kMaceStatsName = "Mace Stats";

// Step 8c. Client Sound is deliberately absent: it has no HUD output, so it must not keep the frame
// pipeline alive - its cues are event-driven and cost nothing while idle.
constexpr const char* kFriendName = "Friend Manager";
constexpr const char* kSafeWalkName = "Safe Walk";
constexpr const char* kRiptideName = "Riptide Indicator";
constexpr const char* kTridentName = "Trident Cooldown";
constexpr const char* kLoyaltyName = "Loyalty HUD";

// Step 8d. The catalogue's last readout: the held item's own item-cooldown.
constexpr const char* kWindChargeName = "Wind Charge CD";

// One enabled check, spelled once. `find` is a name lookup against the fixed registry, so a
// missing name is a null pointer and the dynamic_cast below is a no-op in that case.
template <typename Module>
[[nodiscard]] bool enabled_named(const modules::ModuleManager& registry, const char* name) noexcept {
    const auto* module = dynamic_cast<const Module*>(registry.find(name));
    return module != nullptr && module->enabled();
}

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
    if (enabled_named<modules::visual::CustomCrosshair>(registry, kCrosshairName)
        || enabled_named<modules::movement::VelocityDisplay>(registry, kVelocityName)
        || enabled_named<modules::visual::Trajectories>(registry, kTrajectoriesName)
        || enabled_named<modules::combat::TargetHud>(registry, kTargetHudName)
        || enabled_named<modules::combat::AttackCooldown>(registry, kCooldownName)
        || enabled_named<modules::combat::ReachDisplay>(registry, kReachName)
        || enabled_named<modules::combat::CombatStats>(registry, kCombatStatsName)
        || enabled_named<modules::mace::SmashPotential>(registry, kSmashName)
        || enabled_named<modules::mace::SmashFlash>(registry, kSmashFlashName)
        || enabled_named<modules::mace::MaceStats>(registry, kMaceStatsName)
        || enabled_named<modules::misc::FriendManager>(registry, kFriendName)
        || enabled_named<modules::movement::SafeWalk>(registry, kSafeWalkName)
        || enabled_named<modules::spear::RiptideIndicator>(registry, kRiptideName)
        || enabled_named<modules::spear::TridentCooldown>(registry, kTridentName)
        || enabled_named<modules::spear::LoyaltyHud>(registry, kLoyaltyName)
        || enabled_named<modules::mace::WindChargeCd>(registry, kWindChargeName)) {
        return true;
    }
    return false;
}

} // namespace woke::ui::overlay
