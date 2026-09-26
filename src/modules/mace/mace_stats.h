#pragma once

// Mace Stats (blueprint §8: Mace). Roadmap step 8.
//
// Session counters for mace combat: swings with the mace, hits, and smashes that connected from
// a qualifying fall. Same contract as Combat Stats - the player's own actions counted from
// client-side state, reset through a persisted bind, nothing sent anywhere - with the mace
// detection kept honest: a swing only counts when the game itself reports the held item as the
// mace via its translation key, because inventing an item check would be a guess about a
// version detail. When the key read is unavailable the counters simply stay at zero rather
// than counting the wrong item.
//
// The frame builder supplies the three booleans per swing (is-mace, is-hit, is-smash); the
// module owns only the counters and the reset.

#include <array>
#include <cstdint>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::mace {

class MaceStats final : public BaseModule {
public:
    MaceStats() noexcept
        : BaseModule("Mace Stats",
            "Session counters: mace swings, hits, and landed smashes.",
            Category::Mace, 0) {
        register_settings(std::array<settings::Setting*, 1>{&reset_key_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override { /* counters are session data, like Combat Stats */ }

    [[nodiscard]] std::uint32_t swings() const noexcept { return swings_; }
    [[nodiscard]] std::uint32_t hits() const noexcept { return hits_; }
    [[nodiscard]] std::uint32_t smashes() const noexcept { return smashes_; }

    // One observed left-button press. `was_mace` false keeps every counter at zero, which is
    // what makes the unavailable-key case honest.
    void record_swing(bool was_mace, bool was_hit, bool was_smash) noexcept {
        if (!was_mace) {
            return;
        }
        ++swings_;
        if (was_hit) {
            ++hits_;
        }
        if (was_smash) {
            ++smashes_;
        }
    }

    [[nodiscard]] bool on_key(int virtual_key, bool down) noexcept override {
        if (!down || virtual_key == 0 || virtual_key != reset_key_.value()) {
            return false;
        }
        swings_ = 0;
        hits_ = 0;
        smashes_ = 0;
        return true;
    }

private:
    settings::BindSetting reset_key_{"reset_key", "Key that resets the counters", 0};

    std::uint32_t swings_ = 0;
    std::uint32_t hits_ = 0;
    std::uint32_t smashes_ = 0;
};

} // namespace woke::modules::mace
