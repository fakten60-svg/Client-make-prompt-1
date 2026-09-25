#pragma once

// Fullbright (blueprint §8: Visual). Roadmap step 7.
//
// Writes the game's own gamma option through the game's own SimpleOption setValue path, so the
// option's clamping and change callbacks still run and the game itself sees a value it considers
// legal. Disabling restores whatever the user had, which is why the module never needs to know
// the baseline: the write seam captures it (modules/game_writes.h).
//
// It refuses to enable while the option is unreachable - a stale mapping or no client instance -
// rather than turning on and silently doing nothing (§4.4).

#include <array>

#include "modules/base_module.h"
#include "modules/game_writes.h"
#include "settings/setting.h"

namespace woke::modules::visual {

class Fullbright final : public BaseModule {
public:
    Fullbright() noexcept
        : BaseModule("Fullbright",
            "Raises the gamma option so caves and night read at full brightness.", Category::Visual,
            0) {
        register_settings(std::array<settings::Setting*, 1>{&brightness_});
    }

    bool on_enable() noexcept override {
        if (!game_writes().gamma().valid) {
            return false;
        }
        applied_ = -1.0f;
        apply();
        return true;
    }

    void on_disable() noexcept override {
        game_writes().restore_gamma();
        applied_ = -1.0f;
    }

    // Re-applies only when the slider moved: the gamma option is a plain field the game could
    // reset on a resource reload, but writing it 20 times a second for an unchanged value is
    // waste, and the compare is a float read against cached state.
    void on_tick(float /*delta_seconds*/) noexcept override {
        if (applied_ < 0.0f || value_changed(brightness_.value(), applied_)) {
            apply();
        }
    }

private:
    static bool value_changed(float a, float b) noexcept {
        return (a > b ? a - b : b - a) > 0.0005f;
    }

    void apply() noexcept {
        const float requested = brightness_.value();
        if (game_writes().apply_gamma(requested)) {
            applied_ = requested;
        }
    }

    // The vanilla option's own range: 1.0 is its maximum brightness, so this is fullbright
    // within the values the game will accept rather than a value its validator would clamp.
    settings::SliderSetting brightness_{
        "brightness", "Gamma option value (1.0 is the game's maximum)", 1.0f, 0.0f, 1.0f, 0.05f};
    float applied_ = -1.0f;
};

} // namespace woke::modules::visual
