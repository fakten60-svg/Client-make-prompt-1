#pragma once

// Safe Walk (blueprint §8: Movement). Roadmap step 8.
//
// Sneaking is the game's own "do not walk off this block" behaviour: while the sneak key is held the
// vanilla movement code refuses to step off an edge. Safe Walk therefore holds the game's *own*
// sneak key through the same GameOptions key binding the keyboard drives (modules/game_writes.h) -
// no movement override, no packet, and releasing the toggle restores whatever the player's own key
// was doing. That is the entire feature, and it is why it is an honest assist rather than a hack.
//
// It uses the same re-assert discipline as Auto Sprint: the game re-reads keys every tick and clears
// them on a focus change, so a single write at enable time would quietly stop working after an
// alt-tab. It refuses to enable while the binding is unreachable rather than pretending to help.

#include <array>

#include "modules/base_module.h"
#include "modules/game_writes.h"
#include "settings/setting.h"

namespace woke::modules::movement {

class SafeWalk final : public BaseModule {
public:
    SafeWalk() noexcept
        : BaseModule("Safe Walk",
            "Holds the game's own sneak key so you stop at edges.", Category::Movement, 0) {
        register_settings(std::array<settings::Setting*, 1>{&reassert_});
    }

    bool on_enable() noexcept override {
        if (!game_writes().sneak_key_pressed().valid) {
            return false;
        }
        apply();
        return true;
    }

    void on_disable() noexcept override { game_writes().restore_sneak(); }

    void on_tick(float /*delta_seconds*/) noexcept override {
        if (reassert_.value()) {
            apply();
        }
    }

    // The HUD chip reports the same thing the module does: engaged while enabled.
    [[nodiscard]] bool engaged() const noexcept { return enabled(); }

private:
    void apply() noexcept { (void)game_writes().apply_sneak(true); }

    settings::BoolSetting reassert_{
        "reassert", "Re-hold the key every tick (survives a focus change)", true};
};

} // namespace woke::modules::movement
