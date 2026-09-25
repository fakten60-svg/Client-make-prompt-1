#pragma once

// Config Hotkeys (blueprint §8: Misc). Roadmap step 8.
//
// Three binds, three named profiles: pressing a slot's key loads that profile, and pressing the
// same key again saves the live state back over it. That "press to switch, press again to keep" is
// the whole feature - it turns the config engine into something usable mid-session without opening
// the GUI, and it is the reason the deferred-load mailbox exists in core/config.
//
// Every bind here is a real BindSetting registered with the module, so all three are rebindable from
// the settings drawer and persist exactly like the module's own toggle key does.
//
// The load is *requested*, never performed inline: applying a config flips module enable state, and
// mutating the registry while the key fan-out is walking it is precisely the ordering bug the
// mailbox deferral removes (see core/config.h). The tick gate services it on the game thread.

#include <array>
#include <cstddef>

#include "core/config.h"
#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::misc {

class ConfigHotkeys final : public BaseModule {
public:
    static constexpr std::size_t kSlots = 3;
    static constexpr const char* kProfileNames[kSlots] = {"profile1", "profile2", "profile3"};

    ConfigHotkeys() noexcept
        : BaseModule("Config Hotkeys",
            "Load a saved profile, or press again to save over it.", Category::Misc, 0) {
        register_settings(std::array<settings::Setting*, kSlots>{
            &profile1_key_, &profile2_key_, &profile3_key_});
    }

    bool on_enable() noexcept override { return true; }

    void on_disable() noexcept override { active_profile_ = -1; }

    // The profile keys only act while the module itself is enabled: a hotkey set the user turned
    // off must not keep firing on every press.
    [[nodiscard]] bool on_key(int virtual_key, bool down) noexcept override {
        if (!down || virtual_key == 0 || !enabled()) {
            return false;
        }
        for (std::size_t slot = 0; slot < kSlots; ++slot) {
            if (slot_key(slot) != virtual_key) {
                continue;
            }
            const int slot_index = static_cast<int>(slot);
            if (active_profile_ == slot_index) {
                // Second press on the live profile: keep the session's state as the new profile.
                config::request_save(kProfileNames[slot]);
                active_profile_ = -1;
            } else {
                config::request_load(kProfileNames[slot]);
                active_profile_ = slot_index;
            }
            return true;
        }
        return false;
    }

    // -1 when no profile has been switched to this session. Exposed so the host tests can assert
    // the load/save alternation without reaching into the config engine's mailbox.
    [[nodiscard]] int active_profile() const noexcept { return active_profile_; }

    [[nodiscard]] int slot_key(std::size_t slot) const noexcept {
        const settings::BindSetting* const keys[kSlots] = {&profile1_key_, &profile2_key_,
            &profile3_key_};
        return slot < kSlots ? keys[slot]->value() : 0;
    }

private:
    settings::BindSetting profile1_key_{
        "profile1_key", "Slot 1 profile key (press to load, again to save)", 0};
    settings::BindSetting profile2_key_{
        "profile2_key", "Slot 2 profile key (press to load, again to save)", 0};
    settings::BindSetting profile3_key_{
        "profile3_key", "Slot 3 profile key (press to load, again to save)", 0};

    int active_profile_ = -1;
};

} // namespace woke::modules::misc
