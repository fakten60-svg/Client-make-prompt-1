#pragma once

// Trident Cooldown (blueprint §8: Spear). Roadmap step 8.
//
// The trident's own attack-strength cooldown, read through the game's
// `PlayerEntity.getAttackCooldownProgress(float)` - the exact number 1.21.11 exposes for a weapon's
// swing charge, and the same one the Combat set's Attack Cooldown bar uses. It is shown *only while
// a trident is held*, decided by the held item's own translation key, so the chip is about the
// trident rather than about whatever weapon happens to be in hand.
//
// The module deliberately does not invent a thrown-trident cooldown. Minecraft keeps that timer on
// an ItemCooldownManager the curated mapping asset does not expose, and a client that guessed at its
// duration would be printing a number the game never agreed to. What is shown is real and read-only;
// what is not available degrades to "no chip", never to a wrong figure.

#include <array>
#include <cstddef>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::spear {

class TridentCooldown final : public BaseModule {
public:
    static constexpr std::size_t kColorCount = 5;
    static constexpr const char* kColorLabels[kColorCount] = {"cyan", "white", "green", "red",
        "accent"};

    TridentCooldown() noexcept
        : BaseModule("Trident Cooldown",
            "The trident's own swing cooldown, shown while a trident is held.", Category::Spear, 0) {
        register_settings(std::array<settings::Setting*, 1>{&color_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] std::size_t color_index() const noexcept { return color_.enum_index(); }

private:
    settings::EnumSetting color_{"color", "Chip colour", kColorLabels, 0};
};

} // namespace woke::modules::spear
