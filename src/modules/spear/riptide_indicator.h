#pragma once

// Riptide Indicator (blueprint §8: Spear). Roadmap step 8.
//
// A chip for the riptide state the game itself tracks: `LivingEntity.isUsingRiptide()` is vanilla's
// own "this player is riptiding" flag, and the held item's translation key says whether the weapon
// is a trident. Both are reads; nothing here changes the player or the trident.
//
// The chip is a *state* readout: it is drawn while the module is enabled and the reads resolve, and
// the text distinguishes "a riptide is running" from "a trident is held and ready". The optional
// trident gate exists because a player who is riptiding without a trident in hand cannot be showing
// a trident fact - and the game's flag is the authority on that, not this module.

#include <array>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::spear {

class RiptideIndicator final : public BaseModule {
public:
    RiptideIndicator() noexcept
        : BaseModule("Riptide Indicator",
            "Shows the trident/riptide state the game itself reports.", Category::Spear, 0) {
        register_settings(std::array<settings::Setting*, 1>{&require_trident_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] bool requires_trident() const noexcept { return require_trident_.value(); }

private:
    settings::BoolSetting require_trident_{
        "require_trident", "Only show the chip while a trident is held", true};
};

} // namespace woke::modules::spear
