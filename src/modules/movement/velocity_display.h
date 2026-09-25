#pragma once

// Velocity Display (blueprint §8: Movement). Roadmap step 8.
//
// A readout, not a write: it tells the HUD to draw the player's current movement speed, and the
// unit it should be shown in. The module owns no game state and nothing to restore, so enabling it
// can never fail - the interesting logic is the unit conversion, which is a pure function shared
// with the renderer's frame builder so "what the number means" has exactly one definition.
//
// Horizontal speed, not the velocity vector's magnitude: what a player reads off a speed chip is
// "how fast am I moving across the ground", and including the vertical component would make the
// number disagree with the F3 overlay while falling.

#include <array>
#include <cmath>
#include <cstddef>

#include "jni/game_types.h"
#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::movement {

class VelocityDisplay final : public BaseModule {
public:
    VelocityDisplay() noexcept
        : BaseModule("Velocity Display",
            "Shows the player's current movement speed on the HUD.", Category::Movement, 0) {
        register_settings(std::array<settings::Setting*, 1>{&units_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    static constexpr std::size_t kUnitCount = 3;
    static constexpr const char* kUnitLabels[kUnitCount] = {"m/s", "km/h", "mph"};

    [[nodiscard]] std::size_t units_index() const noexcept { return units_.enum_index(); }

    // The ground speed of a velocity vector, in blocks per second (the game's own unit).
    [[nodiscard]] static double horizontal_speed(const game::Vec3d& velocity) noexcept {
        return std::sqrt((velocity.x * velocity.x) + (velocity.z * velocity.z));
    }

    // The number the chip prints, in whichever unit the enum selected. Unknown indices fall back
    // to metres per second rather than to zero, so a bad index can never show a stopped player.
    [[nodiscard]] static double to_display_unit(
        double meters_per_second, std::size_t unit_index) noexcept {
        switch (unit_index) {
        case 1:
            return meters_per_second * 3.6;
        case 2:
            return meters_per_second * 2.2369362920544;
        default:
            return meters_per_second;
        }
    }

private:
    settings::EnumSetting units_{"units", "Speed unit shown on the HUD", kUnitLabels, 0};
};

} // namespace woke::modules::movement
