#pragma once

// Reach Display (blueprint §8: Combat). Roadmap step 8.
//
// A chip showing the player's *own* entity interaction range - the number the game itself
// consults when deciding whether an attack can connect. It is a fact about your client, not a
// modification of it: the range attribute is what it is, and this module only reads it, which
// is why enabling can never fail. (The blueprint's "on/off" settings row is the module's own
// toggle; anything more would be a write.)
//
// The live read feeds the Target HUD family too: a target card that prints distance becomes
// meaningful next to the range it must fit inside.

#include <array>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::combat {

class ReachDisplay final : public BaseModule {
public:
    ReachDisplay() noexcept
        : BaseModule("Reach Display",
            "Shows the game's own entity interaction range on the HUD.",
            Category::Combat, 0) {
        register_settings(std::array<settings::Setting*, 0>{});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}
};

} // namespace woke::modules::combat
