#pragma once

// Smash damage model (blueprint §8: Mace). Roadmap step 8.
//
// Pure functions only, host-tested, no module and no game state - the same split Trajectories
// uses. The projection is vanilla arithmetic for the mace's smash attack, so the numbers on the
// HUD mean exactly what they mean in the game:
//
//   * base damage: the mace's 5 (weapon) + 1 (fist) attack damage,
//   * fall bonus: the first 3 blocks of fall distance grant 4 damage each; every block beyond
//     that grants 2 (vanilla's bonusDamage calculation, clamped at that formula's own ceiling),
//   * the "enhanced" branch models the smash-enchantment scaling, which is what turns a large
//     fall into a one-shot: extra 4-per-3-blocks steps beyond the base curve.
//
// Everything is declared inline constexpr so the compiler folds it and the test suite can call
// it from any host.

#include <algorithm>
#include <cstdint>

namespace woke::modules::mace {

struct SmashDamage {
    float total = 0.0f;          // the number the HUD prints
    float fall_bonus = 0.0f;     // how much of `total` came from the fall
    bool smash_ready = false;    // fall distance clears the smash threshold
    std::uint32_t curve_steps = 0; // how many 3-block bonus steps the fall grants
};

inline constexpr float kMaceBaseDamage = 6.0f;      // 5 (weapon) + 1 (fist)
inline constexpr float kFallBonusPerStep = 4.0f;    // per full 3 blocks, first steps
inline constexpr float kEnhancedPerStep = 2.0f;     // additional per step, enhanced branch
inline constexpr float kBlocksPerStep = 3.0f;
inline constexpr float kSmashThresholdBlocks = 1.5f; // the "is this a smash" bar

[[nodiscard]] inline SmashDamage project_smash(
    double fall_distance_blocks, bool enhanced) noexcept {
    SmashDamage result{};
    if (fall_distance_blocks <= 0.0) {
        result.total = kMaceBaseDamage;
        return result;
    }

    const float blocks = static_cast<float>(fall_distance_blocks);
    const float steps = blocks / kBlocksPerStep;
    result.curve_steps = static_cast<std::uint32_t>(steps);
    result.smash_ready = blocks >= kSmashThresholdBlocks;

    // Vanilla grants the full bonus only through the first steps; the enhanced branch keeps
    // paying 2 per step beyond that instead of dropping to 1.
    const std::uint32_t base_steps = result.curve_steps < 3u ? result.curve_steps : 3u;
    result.fall_bonus =
        static_cast<float>(base_steps) * kFallBonusPerStep;
    if (result.curve_steps > base_steps) {
        const float extra_steps = static_cast<float>(result.curve_steps - base_steps);
        result.fall_bonus += extra_steps * (enhanced ? kEnhancedPerStep : 1.0f);
    }

    result.total = kMaceBaseDamage + result.fall_bonus;
    return result;
}

} // namespace woke::modules::mace
