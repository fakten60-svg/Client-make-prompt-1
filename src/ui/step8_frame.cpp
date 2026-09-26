#include "ui/step8_frame.h"

// The readout frame fill, Windows-only (see the header). The JNI facade is the only game access;
// every module setting consulted here is a plain value read.

#include <cstddef>
#include <cstring>

#include "core/build_config.h"
#include "core/event_bus.h"
#include "core/events.h"
#include "modules/combat/attack_cooldown.h"
#include "modules/combat/combat_stats.h"
#include "modules/combat/reach_display.h"
#include "modules/combat/target_hud.h"
#include "modules/mace/mace_stats.h"
#include "modules/mace/smash_damage.h"
#include "modules/mace/smash_flash.h"
#include "modules/mace/smash_potential.h"
#include "modules/misc/friend_manager.h"
#include "modules/module_manager.h"
#include "modules/movement/safe_walk.h"
#include "modules/spear/loyalty_hud.h"
#include "modules/spear/riptide_indicator.h"
#include "modules/spear/trident_cooldown.h"

#if WOKE_HAVE_JNI
#include "jni/game_instance.h"
#endif

namespace woke::ui {
namespace {

using modules::combat::AttackCooldown;
using modules::combat::CombatStats;
using modules::combat::ReachDisplay;
using modules::combat::TargetHud;
using modules::mace::MaceStats;
using modules::mace::SmashFlash;
using modules::mace::SmashPotential;
using modules::misc::FriendManager;
using modules::movement::SafeWalk;
using modules::spear::LoyaltyHud;
using modules::spear::RiptideIndicator;
using modules::spear::TridentCooldown;

namespace mace = modules::mace;

// The overlay module pointers, re-resolved on the registry's size-change cadence exactly like
// gui.cpp's own cache. They are non-const because the counters and the loyalty state machine are
// mutated by the swing observer and the per-frame observation.
struct Step8Modules {
    TargetHud* target_hud = nullptr;
    AttackCooldown* cooldown = nullptr;
    ReachDisplay* reach = nullptr;
    CombatStats* combat_stats = nullptr;
    SmashPotential* smash = nullptr;
    SmashFlash* flash = nullptr;
    MaceStats* mace_stats = nullptr;
    FriendManager* friends = nullptr;
    SafeWalk* safe_walk = nullptr;
    RiptideIndicator* riptide = nullptr;
    TridentCooldown* trident = nullptr;
    LoyaltyHud* loyalty = nullptr;
    std::size_t resolved_count = static_cast<std::size_t>(-1);
};

Step8Modules g_step8{};
events::Subscription g_mouse_subscription{};
bool g_mouse_subscribed = false;

void resolve_step8_modules() noexcept {
    const std::size_t count = modules::manager().count();
    if (count == g_step8.resolved_count) {
        return;
    }
    g_step8.resolved_count = count;
    // dynamic_cast, not static_cast: the lookup is by name, so the cast is only sound while that
    // name still maps to the type it claims to. A future name collision becomes a null pointer - a
    // chip that does not draw - instead of undefined behaviour.
    g_step8.target_hud = dynamic_cast<TargetHud*>(modules::manager().find("Target HUD"));
    g_step8.cooldown = dynamic_cast<AttackCooldown*>(modules::manager().find("Attack Cooldown"));
    g_step8.reach = dynamic_cast<ReachDisplay*>(modules::manager().find("Reach Display"));
    g_step8.combat_stats = dynamic_cast<CombatStats*>(modules::manager().find("Combat Stats"));
    g_step8.smash = dynamic_cast<SmashPotential*>(modules::manager().find("Smash Potential"));
    g_step8.flash = dynamic_cast<SmashFlash*>(modules::manager().find("Smash Flash"));
    g_step8.mace_stats = dynamic_cast<MaceStats*>(modules::manager().find("Mace Stats"));
    g_step8.friends = dynamic_cast<FriendManager*>(modules::manager().find("Friend Manager"));
    g_step8.safe_walk = dynamic_cast<SafeWalk*>(modules::manager().find("Safe Walk"));
    g_step8.riptide =
        dynamic_cast<RiptideIndicator*>(modules::manager().find("Riptide Indicator"));
    g_step8.trident =
        dynamic_cast<TridentCooldown*>(modules::manager().find("Trident Cooldown"));
    g_step8.loyalty = dynamic_cast<LoyaltyHud*>(modules::manager().find("Loyalty HUD"));
}

// The two chips that need no game state: the friend list size and the safe-walk engagement are
// module facts, so they are filled even on a build without the JVM bridge.
void read_local_chips(hud::Frame& frame) noexcept {
    if (g_step8.friends != nullptr && g_step8.friends->enabled()) {
        frame.friend_chip = true;
        frame.friend_count = static_cast<std::uint32_t>(g_step8.friends->count());
    }
    if (g_step8.safe_walk != nullptr && g_step8.safe_walk->enabled()) {
        frame.safe_walk_chip = true;
        frame.safe_walk_engaged = g_step8.safe_walk->engaged();
    }
}

#if WOKE_HAVE_JNI

// The trident's own translation key. Comparing the item's own key is the game answering "which item
// is this", instead of the client guessing at a version-specific item class.
constexpr const char* kTridentKey = "item.minecraft.trident";

[[nodiscard]] bool is_trident_key(const game::FixedName& key) noexcept {
    return std::strcmp(key.c_str(), kTridentKey) == 0;
}

// One observed left-button press, recorded against the enabled counters. Order matters: the hit
// extends the swing that was just counted, exactly as the vanilla attack does, and a swing below
// full strength is the one the game would have capped.
void record_swing() noexcept {
    const auto cooldown = game::attack_cooldown_progress();
    const bool below_full_strength = !cooldown.valid || cooldown.value < 1.0f;
    const auto target = game::crosshair_target();
    const bool was_hit = target.valid;

    if (g_step8.combat_stats != nullptr && g_step8.combat_stats->enabled()) {
        g_step8.combat_stats->record_swing(below_full_strength);
        if (was_hit) {
            g_step8.combat_stats->record_hit();
        }
    }

    if (g_step8.mace_stats != nullptr && g_step8.mace_stats->enabled()) {
        const auto mace = game::holding_mace();
        const bool was_mace = mace.valid && mace.value;
        bool was_smash = false;
        if (was_mace) {
            const auto fall = game::player_fall_distance();
            was_smash = fall.valid && mace::project_smash(fall.value, false).smash_ready;
        }
        g_step8.mace_stats->record_swing(was_mace, was_hit, was_smash);
    }
}

// The button number is the window procedure's own mapping: 0 is the left button. A GUI-consumed
// click (the overlay is open) never counts as a swing - it was a click on our own window.
void on_mouse_event(events::MouseEvent& event) {
    if (event.consumed || event.button != 0 || !event.down) {
        return;
    }
    resolve_step8_modules();
    record_swing();
}

void ensure_mouse_subscription() noexcept {
    if (g_mouse_subscribed) {
        return;
    }
    g_mouse_subscribed = true;
    g_mouse_subscription = events::bus().subscribe<events::MouseEvent, &on_mouse_event>();
}

void read_target_card(hud::Frame& frame) noexcept {
    if (g_step8.target_hud == nullptr || !g_step8.target_hud->enabled()) {
        return;
    }
    const auto target = game::crosshair_target();
    if (!target.valid) {
        return;
    }
    frame.target_card = true;
    frame.target_position = g_step8.target_hud->position_index();
    frame.target_scale = g_step8.target_hud->scale();
    frame.target = target.value;
    frame.world_live = true;
}

void read_cooldown(hud::Frame& frame) noexcept {
    if (g_step8.cooldown == nullptr || !g_step8.cooldown->enabled()) {
        return;
    }
    const auto progress = game::attack_cooldown_progress();
    if (!progress.valid) {
        return;
    }
    frame.cooldown_bar = true;
    frame.cooldown_style = g_step8.cooldown->style_index();
    frame.cooldown_color = hud::accent_color_for(g_step8.cooldown->color_index());
    frame.cooldown_progress = progress.value;
    frame.world_live = true;
}

void read_reach(hud::Frame& frame) noexcept {
    if (g_step8.reach == nullptr || !g_step8.reach->enabled()) {
        return;
    }
    const auto range = game::player_attack_range();
    if (!range.valid) {
        return;
    }
    frame.reach_chip = true;
    frame.reach_blocks = range.value;
    frame.world_live = true;
}

void read_smash(hud::Frame& frame) noexcept {
    if (g_step8.smash == nullptr || !g_step8.smash->enabled()) {
        return;
    }
    const auto fall = game::player_fall_distance();
    if (!fall.valid) {
        return;
    }
    frame.smash_chip = true;
    frame.smash_enhanced = g_step8.smash->enhanced();
    frame.fall_distance = fall.value;
    frame.world_live = true;
}

void read_smash_flash(hud::Frame& frame) noexcept {
    if (g_step8.flash == nullptr || !g_step8.flash->enabled()) {
        return;
    }
    const auto fall = game::player_fall_distance();
    if (!fall.valid) {
        return;
    }
    // The flash is the threshold crossing made visible: the same read as the chip, one predicate.
    // The flash has no enhanced setting of its own, so the plain curve decides readiness.
    const mace::SmashDamage projected = mace::project_smash(fall.value, false);
    if (!projected.smash_ready) {
        return;
    }
    frame.smash_flash = true;
    frame.flash_intensity = g_step8.flash->intensity();
    frame.world_live = true;
}

void read_counters(hud::Frame& frame) noexcept {
    // Counters are session data: they draw with or without a live world, so they are the only
    // step-8 fields that do not set world_live.
    if (g_step8.combat_stats != nullptr && g_step8.combat_stats->enabled()) {
        frame.combat_counters = true;
        frame.counter_swings = g_step8.combat_stats->swings();
        frame.counter_hits = g_step8.combat_stats->hits();
        frame.counter_wasted = g_step8.combat_stats->wasted();
    }
    if (g_step8.mace_stats != nullptr && g_step8.mace_stats->enabled()) {
        frame.mace_counters = true;
        frame.mace_swing_count = g_step8.mace_stats->swings();
        frame.mace_hit_count = g_step8.mace_stats->hits();
        frame.mace_smash_count = g_step8.mace_stats->smashes();
    }
}

void read_riptide(hud::Frame& frame, const game::Maybe<game::FixedName>& held) noexcept {
    if (g_step8.riptide == nullptr || !g_step8.riptide->enabled()) {
        return;
    }
    const bool trident = held.valid && is_trident_key(held.value);
    if (g_step8.riptide->requires_trident() && !trident) {
        return; // the chip is a trident fact; no trident, no chip
    }
    const auto active = game::riptide_active();
    if (!active.valid) {
        return;
    }
    frame.riptide_chip = true;
    frame.riptide_engaged = active.value;
    frame.riptide_trident = trident;
    frame.world_live = true;
}

void read_trident(hud::Frame& frame, const game::Maybe<game::FixedName>& held) noexcept {
    if (g_step8.trident == nullptr || !g_step8.trident->enabled()) {
        return;
    }
    if (!held.valid || !is_trident_key(held.value)) {
        return; // only while a trident is held: the chip is about the trident
    }
    const auto progress = game::attack_cooldown_progress();
    if (!progress.valid) {
        return;
    }
    frame.trident_chip = true;
    frame.trident_progress = progress.value;
    frame.trident_color = hud::accent_color_for(g_step8.trident->color_index());
    frame.world_live = true;
}

void read_loyalty(hud::Frame& frame, const game::Maybe<game::FixedName>& held) noexcept {
    if (g_step8.loyalty == nullptr || !g_step8.loyalty->enabled()) {
        return;
    }
    if (!held.valid) {
        return; // an unresolved held-item read must not be mistaken for "the trident is out"
    }
    g_step8.loyalty->observe(static_cast<double>(frame.delta_seconds), is_trident_key(held.value));
    frame.loyalty_chip = true;
    frame.loyalty_tracking = g_step8.loyalty->tracking();
    frame.loyalty_seconds = g_step8.loyalty->elapsed();
    frame.loyalty_last_trip = g_step8.loyalty->last_trip();
    frame.world_live = true;
}

#endif // WOKE_HAVE_JNI

} // namespace

void fill_step8_readouts(hud::Frame& frame) noexcept {
    resolve_step8_modules();
    read_local_chips(frame);
#if WOKE_HAVE_JNI
    ensure_mouse_subscription();
    read_target_card(frame);
    read_cooldown(frame);
    read_reach(frame);
    read_smash(frame);
    read_smash_flash(frame);
    read_counters(frame);

    // One held-item read per frame, shared by the three spear chips: three JNI calls for one fact
    // would be waste, and the reads must agree with each other within a frame.
    const game::Maybe<game::FixedName> held = game::held_item_key();
    read_riptide(frame, held);
    read_trident(frame, held);
    read_loyalty(frame, held);
#endif
}

void shutdown_step8() noexcept {
    if (!g_mouse_subscribed) {
        return;
    }
    events::bus().unsubscribe(g_mouse_subscription);
    g_mouse_subscription = events::Subscription{};
    g_mouse_subscribed = false;
}

} // namespace woke::ui
