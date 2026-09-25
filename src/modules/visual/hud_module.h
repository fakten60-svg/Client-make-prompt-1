#pragma once

// HUD (blueprint §8, §7.6: Visual). Roadmap step 7.
//
// The module is configuration and state only; the drawing lives in ui/hud.cpp, which the overlay
// composition calls with a resolved frame. That split is deliberate: the HUD must render on a
// frame where the ClickGUI is closed, so it cannot be a per-module on_render call - it is part of
// the overlay's own frame pipeline, and it keeps the enabled flag authoritative for both drawing
// and the §7.8 suppression predicate.

#include <array>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::visual {

class HudModule final : public BaseModule {
public:
    HudModule() noexcept
        : BaseModule("HUD", "Watermark and an enabled-module list at the screen edge.",
            Category::Visual, 0) {
        register_settings(std::array<settings::Setting*, 3>{&watermark_, &arraylist_, &sort_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] bool watermark() const noexcept { return watermark_.value(); }
    [[nodiscard]] bool arraylist() const noexcept { return arraylist_.value(); }
    [[nodiscard]] bool sort_by_length() const noexcept { return sort_.enum_index() == 1; }

    // True when this module would draw something. The overlay composition asks before it bothers
    // to fill a frame, and ui::needs_render() asks while the chrome is hidden.
    [[nodiscard]] bool draws() const noexcept { return enabled() && (watermark() || arraylist()); }

private:
    settings::BoolSetting watermark_{"watermark", "Show the client watermark", true};
    settings::BoolSetting arraylist_{"arraylist", "List enabled modules at the screen edge", true};

    static constexpr const char* kSorts[] = {"name", "length"};
    settings::EnumSetting sort_{"sort", "Arraylist ordering", kSorts, 0};
};

} // namespace woke::modules::visual
