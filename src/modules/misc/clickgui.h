#pragma once

// ClickGUI (blueprint §8: Misc). Roadmap step 7.
//
// The overlay owns the toggle key: RSHIFT is consumed by the GUI's own key handler, one boot step
// before the module dispatcher, so the key never reaches the game and never has to be routed to a
// module bind. This module exists so the GUI is a first-class, listed, persisted entry in the
// Misc category: enabling it opens the overlay, disabling it closes it.
//
// Because the overlay can also be closed from inside itself (the red traffic light, or losing
// focus), gui.cpp mirrors that visibility change back here. That round trip is what keeps the
// card's pill and the window from ever disagreeing (§6, ModuleCard's invariant).

#ifndef _WIN32
#error "clickgui.h is Windows-only; it drives the overlay."
#endif

#include "modules/base_module.h"
#include "settings/setting.h"
#include "ui/gui.h"

namespace woke::modules::misc {

class ClickGUI final : public BaseModule {
public:
    ClickGUI() noexcept
        : BaseModule("ClickGUI", "Open or close the ClickGUI overlay.", Category::Misc, 0) {
        register_settings(std::array<settings::Setting*, 1>{&hold_});
    }

    bool on_enable() noexcept override {
        ui::set_visible(true);
        return true;
    }

    void on_disable() noexcept override { ui::set_visible(false); }

    // Called by the overlay after it changed its own visibility. set_enabled() re-enters
    // on_enable/on_disable, which call ui::set_visible again; that call is a no-op because the
    // value already matches, so the round trip terminates after one step.
    void sync_visibility(bool visible) noexcept {
        if (visible != enabled()) {
            set_enabled(visible);
        }
    }

    [[nodiscard]] bool hold_mode() const noexcept { return hold_.value(); }

    // Step 8 wires the setting through the dispatcher's hold path: with a bind set, the GUI opens
    // on the press edge and closes on the release edge, and with no bind the behaviour is unchanged
    // (the overlay's own toggle key, handled one boot step earlier, remains the primary affordance).
    [[nodiscard]] bool hold_to_activate() const noexcept override { return hold_.value(); }

private:
    settings::BoolSetting hold_{"hold_mode", "Keep the GUI open only while the key is held", false};
};

} // namespace woke::modules::misc
