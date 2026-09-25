#pragma once

// Panic (blueprint §8: Misc). Roadmap step 8.
//
// The one module whose job is to stop everything: its bind disables every enabled module in a
// single keypress, and the card's own toggle does nothing, because a "Panic that is on" is not a
// state - it is an event. That is what is_action() exists for: the GUI draws no switch for it and
// the keybind toast stays silent, since "Panic: off" is a sentence that should never reach a user.
//
// With no settings of its own it still persists one thing - its bind - because BaseModule registers
// the bind as a setting for every module (see base_module.h).
//
// Unbound by default on purpose: a panic key the user did not choose is a panic key they will hit
// by accident, and the card gives them a one-click way to arm it.

#include "modules/base_module.h"
#include "modules/module_manager.h"
#include "settings/setting.h"

namespace woke::modules::misc {

class Panic final : public BaseModule {
public:
    Panic() noexcept
        : BaseModule("Panic", "Disable every enabled module at once.", Category::Misc, 0) {}

    // There is no enabled state to hold: enable() refuses, so the pill can never claim "on".
    bool on_enable() noexcept override { return false; }
    void on_disable() noexcept override {}

    [[nodiscard]] bool is_action() const noexcept override { return true; }

    // Fires once, on the press edge only. The key is claimed even when nothing was live, so the
    // game never also sees the panic key.
    [[nodiscard]] bool on_key(int virtual_key, bool down) noexcept override {
        if (!down || virtual_key == 0 || virtual_key != bind()) {
            return false;
        }
        (void)manager().disable_all();
        return true;
    }
};

} // namespace woke::modules::misc
