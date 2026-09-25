#pragma once

// Module registry (blueprint §4.4).
//
// A fixed pool filled once at boot, never resized, never reordered: module indices are stable for
// the whole session, which is what lets the GUI and the config engine refer to a module by index.
// Category badge counts are precomputed on registration and recomputed only on enable/disable -
// the sidebar reads two ints per frame.
//
// Portable on purpose (no windows.h): the pool, the buckets and the keybind dispatch are plain
// logic, so the host test suite covers registration, fan-out, bind routing and panic without a
// game. File IO for the config engine lives in core/config.

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/base_module.h"
#include "modules/category.h"

namespace woke::modules {

class ModuleManager {
public:
    static constexpr std::size_t kMaxModules = 32;

    // Takes ownership. Registration order is display order within a category.
    [[nodiscard]] bool add(BaseModule* module) noexcept;

    // Drops every module without enabling/disabling them again: used only at shutdown, where
    // disable side effects are already handled by lifecycle ordering.
    void reset() noexcept;

    [[nodiscard]] BaseModule* find(std::string_view name) const noexcept;
    [[nodiscard]] BaseModule* at(std::size_t index) const noexcept;
    [[nodiscard]] std::size_t count() const noexcept { return count_; }

    [[nodiscard]] std::size_t category_total(Category category) const noexcept;
    [[nodiscard]] std::size_t category_enabled(Category category) const noexcept;

    // Enables or disables everything in a category; used by panic's inverse and future group UI.
    [[nodiscard]] std::size_t set_category_enabled(Category category, bool enabled) noexcept;

    // Panic (§8): disables every module. Returns how many were live.
    [[nodiscard]] std::size_t disable_all() noexcept;

    // Keybind routing (§4.4, extended in step 8). Both edges are forwarded to every module's
    // on_key, so hold-mode and action modules work through the same single path the press-toggle
    // default uses. Returns true when any module claimed the key.
    [[nodiscard]] bool handle_key(int virtual_key, bool down) noexcept;

    // True when some *toggle* module is bound to this key, i.e. when the keypress changed a state
    // worth reporting. Action modules are excluded: their key does something rather than flipping a
    // state, so the toast layer must not announce a module that was never on.
    [[nodiscard]] bool has_toggle_bind(int virtual_key) const noexcept;

    // Per-tick and per-frame fan-out over enabled modules only.
    void on_tick(float delta_seconds) noexcept;
    void on_render(float delta_seconds) noexcept;

    // Recomputes the category buckets after a direct module mutation (a toggle through the GUI, a
    // config load). Cheap: one pass over at most 32 modules.
    void notify_changed() noexcept;

    [[nodiscard]] std::size_t enabled_count() const noexcept;

private:
    void recompute_buckets() noexcept;

    std::array<BaseModule*, kMaxModules> modules_{};
    std::size_t count_ = 0;
    std::array<std::size_t, kCategoryCount> totals_{};
    std::array<std::size_t, kCategoryCount> enabled_{};
};

// The process-wide registry, owned by module_manager.cpp. Boot fills it once; every other system
// (config, GUI) reads through here.
[[nodiscard]] ModuleManager& manager() noexcept;

} // namespace woke::modules
