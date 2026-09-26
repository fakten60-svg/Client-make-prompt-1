#pragma once

// Smash Potential (blueprint §8: Mace). Roadmap step 8.
//
// "What would this fall do?" - the projected smash damage of the player's current fall distance,
// printed as a chip and recoloured once the number clears the smash threshold. The read is the
// player's own fallDistance field; the projection is the pure model in smash_damage.h. Nothing
// here touches the mace, the swing, or the landing - it is arithmetic about the fall you are
// already in, which is why it is a readout module that enables anywhere.
//
// The chip colours by damage band through the shared theme palette: normal, notable (>= 12,
// roughly a half-heart clip on an unarmoured player), and lethal (>= 20). The renderer owns the
// actual swatches; this file owns the thresholds.

#include <array>
#include <cstddef>

#include "modules/base_module.h"
#include "modules/mace/smash_damage.h"
#include "settings/setting.h"

namespace woke::modules::mace {

class SmashPotential final : public BaseModule {
public:
    static constexpr std::size_t kBandCount = 3;
    static constexpr float kNotableDamage = 12.0f;
    static constexpr float kLethalDamage = 20.0f;

    SmashPotential() noexcept
        : BaseModule("Smash Potential",
            "Projected mace smash damage from your current fall.",
            Category::Mace, 0) {
        register_settings(std::array<settings::Setting*, 1>{&enhanced_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    // Damage band for the colour swatch: 0 normal, 1 notable, 2 lethal. Unknown values clamp.
    [[nodiscard]] static std::size_t band_for(float damage) noexcept {
        if (damage >= kLethalDamage) {
            return 2;
        }
        return damage >= kNotableDamage ? 1u : 0u;
    }

    [[nodiscard]] bool enhanced() const noexcept { return enhanced_.value(); }

private:
    settings::BoolSetting enhanced_{
        "enhanced", "Model the enhanced (smash-enchanted) curve", false};
};

} // namespace woke::modules::mace
