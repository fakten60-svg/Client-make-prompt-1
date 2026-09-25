#pragma once

// Trajectories (blueprint §8: Visual). Roadmap step 7.
//
// Client-side projectile path prediction. The physics integration and the projection are in
// trajectory.h (pure, host-tested); this module supplies the parameters and the colour, and
// ui/hud.cpp draws the result. Nothing here consults the world for collisions, which is why the
// module is honest about being a prediction: the settings describe the projectile, and the path
// is what those numbers imply.

#include <array>
#include <cstddef>

#include "modules/base_module.h"
#include "modules/visual/trajectory.h"
#include "settings/setting.h"
#include "ui/theme.h"

namespace woke::modules::visual {

class Trajectories final : public BaseModule {
public:
    Trajectories() noexcept
        : BaseModule("Trajectories", "Predicts the flight path of the held projectile.",
            Category::Visual, 0) {
        register_settings(std::array<settings::Setting*, 4>{&gravity_, &drag_, &seconds_, &color_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] TrajectoryParams params() const noexcept {
        TrajectoryParams params{};
        params.gravity = gravity_.value();
        params.drag = drag_.value();
        params.seconds = seconds_.value();
        return params;
    }

    [[nodiscard]] std::size_t color_index() const noexcept { return color_.enum_index(); }

    [[nodiscard]] static util::Rgba color_for(std::size_t index) noexcept {
        switch (index) {
        case 1:
            return ui::theme::color::kText;
        case 2:
            return ui::theme::color::kTrafficGreen;
        case 3:
            return ui::theme::color::kTrafficRed;
        case 4:
            return ui::theme::color::kAccentAlt;
        default:
            return ui::theme::color::kAccent;
        }
    }

private:
    // An arrow's defaults: 0.05 blocks/tick^2 of gravity and 0.99 per-tick retention.
    settings::SliderSetting gravity_{"gravity", "Gravity per tick (0.05 is an arrow)", 0.05f, 0.02f,
        0.12f, 0.01f};
    settings::SliderSetting drag_{"drag", "Per-tick velocity retention", 0.99f, 0.90f, 1.0f, 0.01f};
    settings::SliderSetting seconds_{"seconds", "How far ahead to predict", 3.0f, 1.0f, 6.0f, 0.5f};

    static constexpr const char* kColors[] = {"accent", "white", "green", "red", "cyan"};
    settings::EnumSetting color_{"color", "Path colour", kColors, 0};
};

} // namespace woke::modules::visual
