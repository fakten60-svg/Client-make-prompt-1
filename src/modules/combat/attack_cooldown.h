#pragma once

// Attack Cooldown (blueprint §8: Combat). Roadmap step 8.
//
// The progress bar under the crosshair, fed by the game's own attack-cooldown clock
// (PlayerEntity.getAttackCooldownProgress) - the exact number the vanilla HUD bar uses, so
// "full" here means "the next swing does full damage" exactly when the game means it.
//
// Style 0 is a thin bar under the crosshair, style 1 an arc around it. Both are drawn from the
// frame; the module only supplies the colour intent (shared theme palette, like Trajectories).

#include <array>
#include <cstddef>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::combat {

class AttackCooldown final : public BaseModule {
public:
    static constexpr std::size_t kStyleCount = 2;
    static constexpr const char* kStyleLabels[kStyleCount] = {"bar", "arc"};

    static constexpr std::size_t kColorCount = 5;
    static constexpr const char* kColorLabels[kColorCount] = {"accent", "white", "green", "red",
        "cyan"};

    AttackCooldown() noexcept
        : BaseModule("Attack Cooldown",
            "Attack-strength progress bar under the crosshair.", Category::Combat, 0) {
        register_settings(std::array<settings::Setting*, 2>{&style_, &color_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] std::size_t style_index() const noexcept { return style_.enum_index(); }
    [[nodiscard]] std::size_t color_index() const noexcept { return color_.enum_index(); }

private:
    settings::EnumSetting style_{"style", "Bar shape", kStyleLabels, 0};

    settings::EnumSetting color_{"color", "Bar colour", kColorLabels, 0};
};

} // namespace woke::modules::combat
