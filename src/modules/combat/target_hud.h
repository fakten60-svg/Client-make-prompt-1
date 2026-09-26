#pragma once

// Target HUD (blueprint §8: Combat). Roadmap step 8.
//
// A card about whatever the *game* put under the crosshair: name, health, distance. The module
// owns no target of its own - it configures the card and answers "draw it here" - because a
// client that chose its own target would already be past what a readout should do. The reads
// happen in the frame builder on the game thread; on_tick does nothing.
//
// Position settings are intent, not geometry: 0 anchors the card at screen centre under the
// crosshair, 1 at the top-left chip column. The renderer owns the actual rectangles, so a
// re-theme never has to touch this file.

#include <array>
#include <cstddef>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::combat {

class TargetHud final : public BaseModule {
public:
    static constexpr std::size_t kPositionCount = 2;
    static constexpr const char* kPositionLabels[kPositionCount] = {"crosshair", "corner"};

    TargetHud() noexcept
        : BaseModule("Target HUD",
            "Card for the entity under your crosshair: name, health, distance.",
            Category::Combat, 0) {
        register_settings(std::array<settings::Setting*, 2>{&position_, &scale_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] std::size_t position_index() const noexcept { return position_.enum_index(); }
    [[nodiscard]] float scale() const noexcept { return scale_.value(); }

private:
    settings::EnumSetting position_{
        "position", "Where the card sits", kPositionLabels, 0};
    settings::SliderSetting scale_{
        "scale", "Card scale", 1.0f, 0.75f, 1.5f, 0.05f};
};

} // namespace woke::modules::combat
