#pragma once

// Combat Stats (blueprint §8: Combat). Roadmap step 8.
//
// Session counters: swings, hits, cooldown wastes. Counted from information the client already
// has - the left mouse button's press edge (via the event bus) and the game's own cooldown
// number - with no server round-trip and no knowledge of *why* a swing happened.
//
// The counting rules, stated once:
//   * a swing is a fresh (non-repeat) left-button press while a world is live,
//   * a "wasted" swing is a press made below the game's full-strength threshold
//     (attack_cooldown_progress() < 1.0 at the moment of the press),
//   * a hit is a swing taken while the game itself reports an entity under the crosshair.
// These are the player's own actions counted client-side - the module never presses a button
// for anyone, which is why it is a readout module with a bind, not a writer.
//
// The reset bind is the blueprint's "reset bind" settings row, wired through the standard
// on_key path so it rebinds and persists like every other bind in the client.

#include <array>
#include <cstdint>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::combat {

class CombatStats final : public BaseModule {
public:
    CombatStats() noexcept
        : BaseModule("Combat Stats",
            "Session counters: swings, hits, swings below full strength.",
            Category::Combat, 0) {
        register_settings(std::array<settings::Setting*, 1>{&reset_key_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override { /* counters survive a toggle: they are session data */ }

    // The four counters the HUD chip prints. `wasted` counts swings the game would have capped;
    // it exists so the number can be *useful*, not to judge anyone's clicking.
    [[nodiscard]] std::uint32_t swings() const noexcept { return swings_; }
    [[nodiscard]] std::uint32_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::uint32_t wasted() const noexcept { return wasted_; }

    // Input intake, called by the frame side with what it already observed. Order matters:
    // record_hit() extends the press that record_swing() counted, so a hit is always also a
    // swing - exactly how the vanilla attack works.
    void record_swing(bool below_full_strength) noexcept {
        ++swings_;
        if (below_full_strength) {
            ++wasted_;
        }
    }

    void record_hit() noexcept { ++hits_; }

    void on_tick(float /*delta_seconds*/) noexcept override {}

    [[nodiscard]] bool on_key(int virtual_key, bool down) noexcept override {
        if (!down || virtual_key == 0 || virtual_key != reset_key_.value()) {
            return false;
        }
        swings_ = 0;
        hits_ = 0;
        wasted_ = 0;
        return true;
    }

private:
    // Unbound by default: a reset key someone did not choose would wipe session data by
    // accident. The card arms it with one click, like Panic's bind.
    settings::BindSetting reset_key_{"reset_key", "Key that resets the counters", 0};

    std::uint32_t swings_ = 0;
    std::uint32_t hits_ = 0;
    std::uint32_t wasted_ = 0;
};

} // namespace woke::modules::combat
