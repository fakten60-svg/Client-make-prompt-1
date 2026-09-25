#pragma once

// Auto Sprint (blueprint §8: Movement). Roadmap step 8.
//
// This module holds the game's *own* sprint key, through the same GameOptions key binding the
// player's keyboard drives (modules/game_writes.h). Nothing here synthesizes a packet or writes a
// movement flag: the vanilla input pipeline sees the key down, its own canSprint / shouldStop*
// checks decide what that means this tick, and the game sends whatever it would have sent anyway.
// That is what makes it a QoL toggle rather than a movement hack, and it is why disabling simply
// releases the key and leaves the player in whatever state the keyboard says.
//
// The 1.21.11 mapping makes this the *only* honest option: ClientPlayerEntity in this version has
// no setSprinting, so the alternatives would all be packet synthesis, which the client's contract
// forbids (§1: strictly client-side, no traffic of our own).
//
// It refuses to enable while the binding is unreachable - a stale mapping, or no client instance -
// rather than turning on and silently doing nothing (§4.4).

#include <array>

#include "modules/base_module.h"
#include "modules/game_writes.h"
#include "settings/setting.h"

namespace woke::modules::movement {

class AutoSprint final : public BaseModule {
public:
    AutoSprint() noexcept
        : BaseModule("Auto Sprint",
            "Holds the game's own sprint key so you always sprint while moving.",
            Category::Movement, 0) {
        register_settings(std::array<settings::Setting*, 1>{&reassert_});
    }

    bool on_enable() noexcept override {
        if (!game_writes().sprint_key_pressed().valid) {
            return false;
        }
        apply();
        return true;
    }

    void on_disable() noexcept override { game_writes().restore_sprint(); }

    // The game clears its key bindings on a focus change and re-reads the physical keys every tick,
    // so a single write at enable time would quietly stop working after an alt-tab. Re-asserting is
    // one write per tick, which is the same order of cost as the key handler the player's own
    // finger drives - and it is skippable for anyone who wants the one-shot behaviour.
    void on_tick(float /*delta_seconds*/) noexcept override {
        if (reassert_.value()) {
            apply();
        }
    }

private:
    void apply() noexcept { (void)game_writes().apply_sprint(true); }

    settings::BoolSetting reassert_{
        "reassert", "Re-hold the key every tick (survives a focus change)", true};
};

} // namespace woke::modules::movement
