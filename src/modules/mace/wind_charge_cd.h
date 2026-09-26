#pragma once

// Wind Charge CD (blueprint §8: Mace). Roadmap step 8 - the catalogue's last open entry.
//
// The held item's own item-cooldown, read through the game's ItemCooldownManager - the same
// manager the vanilla cooldown overlay ticks - so a thrown wind charge's timer is the game's own
// number, never a client-side guess at its duration. Progress is 0..1 where 1.0 means "ready",
// which is the manager's own convention for getCooldownProgress().
//
// The chip is shown *only while a cooldown is actually running* (progress < 1) and, with the
// default "hide when ready" setting, disappears the moment the item is usable again - a chip that
// read "100%" every idle frame would be noise about nothing. An unavailable read degrades to "no
// chip", exactly like every other readout; it never degrades to a wrong figure.

#include <array>
#include <cstddef>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::mace {

class WindChargeCd final : public BaseModule {
public:
    static constexpr std::size_t kColorCount = 5;
    static constexpr const char* kColorLabels[kColorCount] = {"yellow", "white", "green", "red",
        "cyan"};

    WindChargeCd() noexcept
        : BaseModule("Wind Charge CD",
            "The held item's item-cooldown ring value, straight from the game's own manager.",
            Category::Mace, 0) {
        register_settings(std::array<settings::Setting*, 2>{&color_, &hide_ready_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] std::size_t color_index() const noexcept { return color_.enum_index(); }

    // True when the chip should stay hidden at progress 1.0 (the default), so an idle item draws
    // nothing instead of a permanent "100%".
    [[nodiscard]] bool hide_when_ready() const noexcept { return hide_ready_.value(); }

private:
    settings::EnumSetting color_{"color", "Chip colour", kColorLabels, 0};
    settings::BoolSetting hide_ready_{"hide_ready", "Hide when ready", true};
};

} // namespace woke::modules::mace
