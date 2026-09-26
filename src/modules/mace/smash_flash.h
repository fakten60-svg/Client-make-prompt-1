#pragma once

// Smash Flash (blueprint §8: Mace). Roadmap step 8.
//
// A local screen-edge flash for the instant a smash becomes available: the player's own fall
// distance crosses the smash threshold while a mace is the held item. It is a *local* effect -
// four translucent edge quads on the overlay, the same primitives the chrome's shadow uses -
// and it fires on state, not on traffic: nothing is sent, nothing is heard, the screen simply
// glows at you.
//
// The "ready" edge is computed by the renderer from the frame's fall distance and this module's
// threshold; the module owns the look (colour, intensity). Unbound, always-on-while-enabled:
// a flash the user armed should not need a second key.

#include <array>
#include <cstddef>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::mace {

class SmashFlash final : public BaseModule {
public:
    static constexpr std::size_t kColorCount = 5;
    static constexpr const char* kColorLabels[kColorCount] = {"accent", "white", "green", "red",
        "cyan"};

    SmashFlash() noexcept
        : BaseModule("Smash Flash",
            "Screen-edge flash when a smash is ready from your current fall.",
            Category::Mace, 0) {
        register_settings(std::array<settings::Setting*, 2>{&color_, &intensity_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] std::size_t color_index() const noexcept { return color_.enum_index(); }
    [[nodiscard]] float intensity() const noexcept { return intensity_.value(); }

private:
    settings::EnumSetting color_{"color", "Flash colour", kColorLabels, 0};
    settings::SliderSetting intensity_{"intensity", "Flash strength", 0.50f, 0.10f, 1.0f, 0.05f};
};

} // namespace woke::modules::mace
