#pragma once

// Custom Crosshair (blueprint §8: Visual). Roadmap step 7.
//
// A purely local overlay: it draws a crosshair of the user's choosing over the game's own. The
// shape arithmetic lives in crosshair_geometry.h so it is host-testable, and the drawing lives in
// ui/hud.cpp so the module stays configuration and state.

#include <array>
#include <cstddef>

#include "modules/base_module.h"
#include "modules/visual/crosshair_geometry.h"
#include "settings/setting.h"
#include "ui/theme.h"

namespace woke::modules::visual {

class CustomCrosshair final : public BaseModule {
public:
    CustomCrosshair() noexcept
        : BaseModule("Custom Crosshair", "A local crosshair with shape, size and colour control.",
            Category::Visual, 0) {
        register_settings(
            std::array<settings::Setting*, 5>{&shape_, &size_, &gap_, &thickness_, &color_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] CrosshairShape shape() const noexcept {
        return static_cast<CrosshairShape>(shape_.enum_index());
    }
    [[nodiscard]] float size() const noexcept { return size_.value(); }
    [[nodiscard]] float gap() const noexcept { return gap_.value(); }
    [[nodiscard]] float thickness() const noexcept { return thickness_.value(); }

    // Index into kColors; kept as an index so the module header owns the palette and the
    // renderer never has to know which token a name maps to.
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
    static constexpr const char* kShapes[] = {"cross", "dot", "circle", "cross+dot"};
    static_assert(sizeof(kShapes) / sizeof(kShapes[0]) == kCrosshairShapeCount,
        "the shape labels must match the shape enum the renderer switches on");

    settings::EnumSetting shape_{"shape", "Crosshair shape", kShapes, 0};
    settings::SliderSetting size_{"size", "Arm length in pixels", 8.0f, 2.0f, 24.0f, 1.0f};
    settings::SliderSetting gap_{"gap", "Empty space left at the centre", 3.0f, 0.0f, 10.0f, 1.0f};
    settings::SliderSetting thickness_{"thickness", "Line thickness", 1.5f, 1.0f, 4.0f, 0.5f};

    static constexpr const char* kColors[] = {"accent", "white", "green", "red", "cyan"};
    settings::EnumSetting color_{"color", "Crosshair colour", kColors, 0};
};

} // namespace woke::modules::visual
