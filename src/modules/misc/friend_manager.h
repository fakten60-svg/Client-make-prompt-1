#pragma once

// Friend Manager (blueprint §8: Misc). Roadmap step 8.
//
// A local list of names the client treats as friends, plus the nametag colour intent used for them.
// It is deliberately a *local* structure: nothing is sent, nothing is asked of any server, and the
// list has no effect the player cannot see (the HUD chip reports how many entries are active).
//
// Storage is a fixed array of bounded names (game::FixedName), so adding a friend can never
// allocate and a name longer than the buffer truncates rather than overruns - the same discipline
// every other HUD string follows. The list is session data: the setting model is Bool/Slider/Enum/
// Bind, so a name list is not something the config engine can persist yet, and claiming otherwise
// would be a lie. That limitation is stated here rather than hidden in a doc.
//
// The colour is an enum whose indices are matched by the HUD's accent table (hud::accent_color_for),
// exactly like the step-8 Attack Cooldown and Smash Flash colours.

#include <array>
#include <cstddef>

#include "jni/game_types.h"
#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules::misc {

class FriendManager final : public BaseModule {
public:
    static constexpr std::size_t kMaxFriends = 8;

    static constexpr std::size_t kColorCount = 5;
    static constexpr const char* kColorLabels[kColorCount] = {"accent", "white", "green", "red",
        "cyan"};

    FriendManager() noexcept
        : BaseModule("Friend Manager",
            "Local friend list and nametag colour intent (session-scoped).", Category::Misc, 0) {
        register_settings(std::array<settings::Setting*, 1>{&color_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] std::size_t color_index() const noexcept { return color_.enum_index(); }

    // ── The list ─────────────────────────────────────────────────────────────────
    // Append-only ordering with duplicate and full rejection, so the chip's count is stable and an
    // accidental double-add is a no-op rather than a second entry.
    [[nodiscard]] std::size_t count() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

    [[nodiscard]] bool contains(std::string_view name) const noexcept {
        for (std::size_t index = 0; index < count_; ++index) {
            if (same_name(entries_[index], name)) {
                return true;
            }
        }
        return false;
    }

    bool add(std::string_view name) noexcept {
        if (name.empty() || count_ >= kMaxFriends || contains(name)) {
            return false;
        }
        entries_[count_].assign(name);
        ++count_;
        return true;
    }

    bool remove(std::string_view name) noexcept {
        for (std::size_t index = 0; index < count_; ++index) {
            if (!same_name(entries_[index], name)) {
                continue;
            }
            // Shift the tail down: order is not semantic, but a compact array keeps the read path a
            // simple counted loop and the chip's count equal to the number of valid slots.
            for (std::size_t move = index + 1; move < count_; ++move) {
                entries_[move - 1] = entries_[move];
            }
            --count_;
            return true;
        }
        return false;
    }

    void clear() noexcept { count_ = 0; }

    [[nodiscard]] const game::FixedName& entry(std::size_t index) const noexcept {
        return entries_[index];
    }

private:
    [[nodiscard]] static bool same_name(
        const game::FixedName& left, std::string_view right) noexcept {
        if (left.length != right.size()) {
            return false;
        }
        for (std::size_t index = 0; index < left.length; ++index) {
            if (left.text[index] != right[index]) {
                return false;
            }
        }
        return true;
    }

    settings::EnumSetting color_{"color", "Nametag colour intent for friends", kColorLabels, 0};

    std::array<game::FixedName, kMaxFriends> entries_{};
    std::size_t count_ = 0;
};

} // namespace woke::modules::misc
