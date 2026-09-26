#pragma once

// Loyalty HUD (blueprint §8: Spear). Roadmap step 8.
//
// A client-side observation of the trident's throw-and-return trip: while the trident is not in the
// player's main hand the module times how long it has been away, and when it is back it reports the
// completed trip. The signal is the held item's own translation key, so the "thrown" moment is the
// honest one - the trident genuinely left the hand - and no projectile or entity is guessed at.
//
// It is an observation, not a prediction: the module never claims where the trident is or when it
// will land, only how long it has been gone. That is the whole readout, and it is exactly the kind of
// local, user-visible information the client's contract permits.
//
// The state machine is pure and host-tested: observe() is called once per frame with the frame's own
// delta and the current held-trident fact.

#include <array>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::spear {

class LoyaltyHud final : public BaseModule {
public:
    LoyaltyHud() noexcept
        : BaseModule("Loyalty HUD",
            "Times the trident's throw-and-return trip (client-side observation).",
            Category::Spear, 0) {
        register_settings(std::array<settings::Setting*, 1>{&reset_key_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    // One frame's observation. `trident_held` is the held-item read; `delta_seconds` is the frame
    // delta. Away from the hand means "thrown"; back in the hand ends the trip.
    void observe(double delta_seconds, bool trident_held) noexcept {
        const double step = delta_seconds > 0.0 ? delta_seconds : 0.0;
        if (!trident_held) {
            tracking_ = true;
            elapsed_ += step;
            return;
        }
        if (tracking_) {
            last_trip_ = elapsed_;
        }
        tracking_ = false;
        elapsed_ = 0.0;
    }

    [[nodiscard]] bool tracking() const noexcept { return tracking_; }
    [[nodiscard]] double elapsed() const noexcept { return elapsed_; }
    [[nodiscard]] double last_trip() const noexcept { return last_trip_; }

    void reset() noexcept {
        tracking_ = false;
        elapsed_ = 0.0;
        last_trip_ = 0.0;
    }

    [[nodiscard]] bool on_key(int virtual_key, bool down) noexcept override {
        if (!down || virtual_key == 0 || virtual_key != reset_key_.value()) {
            return false;
        }
        reset();
        return true;
    }

private:
    settings::BindSetting reset_key_{"reset_key", "Key that clears the trip timer", 0};

    bool tracking_ = false;
    double elapsed_ = 0.0;
    double last_trip_ = 0.0;
};

} // namespace woke::modules::spear
