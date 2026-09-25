#pragma once

// BaseModule (blueprint §3.4, §4.4).
//
// The contract every module implements. Three rules shape it:
//
//   * A module owns its settings by value, in a fixed array, declared inline in its constructor.
//     Settings self-register into this array (the module's ctor builds it), which is what makes
//     config persistence automatic: the config engine walks module->settings() and needs no
//     per-module code (§4.2's auto-binding invariant).
//   * enable()/disable() is the only mutation path for module state. Subclasses implement
//     on_enable()/on_disable(), which are called only from here - and only after the state flag
//     has flipped, so a hook can never observe a "disabled module that thinks it is enabled".
//   * A constructor never touches the JVM (§4.4). Modules are constructed at boot, before any
//     bridge exists; all game interaction happens in on_tick/on_render through the game façade.

#include <array>
#include <cstddef>
#include <cstdint>

#include "modules/category.h"
#include "settings/setting.h"

namespace woke::modules {

class BaseModule {
public:
    BaseModule(const BaseModule&) = delete;
    BaseModule& operator=(const BaseModule&) = delete;

    virtual ~BaseModule() = default;

    [[nodiscard]] const char* name() const noexcept { return name_; }
    [[nodiscard]] const char* description() const noexcept { return description_; }
    [[nodiscard]] Category category() const noexcept { return category_; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    // Bind that toggles this module (0 = unbound). Step 6 makes it rebindable through the keybind
    // chip's capture mode; persisting the bind alongside the module's settings lands with the
    // Keybinds page in step 8, so a rebind is currently session-scoped.
    [[nodiscard]] int bind() const noexcept { return bind_; }
    void set_bind(int virtual_key) noexcept { bind_ = virtual_key < 0 ? 0 : virtual_key; }

    // The module's settings, in declaration order. Erased on purpose: the config engine and the
    // GUI need "walk the settings", not the array's size at compile time. register_settings()
    // copies the pointers into the module's own fixed storage, so nothing dangles.
    [[nodiscard]] settings::Setting* const* settings() const noexcept {
        return setting_ptrs_.data();
    }
    [[nodiscard]] std::size_t setting_count() const noexcept { return setting_count_; }

    // True when any setting was changed since the last save (§4.4's persist-dirty flag).
    [[nodiscard]] bool dirty() const noexcept {
        for (std::size_t index = 0; index < setting_count_; ++index) {
            if (setting_ptrs_[index]->dirty()) {
                return true;
            }
        }
        return false;
    }

    void clear_dirty() const noexcept {
        for (std::size_t index = 0; index < setting_count_; ++index) {
            setting_ptrs_[index]->clear_dirty();
        }
    }

    // The only mutation path. Returns false when the state did not change (or the module refused
    // in on_enable), so the caller knows whether a config save is warranted.
    bool enable() noexcept {
        if (enabled_) {
            return false;
        }
        enabled_ = true;
        if (!on_enable()) {
            enabled_ = false;
            return false;
        }
        return true;
    }

    bool disable() noexcept {
        if (!enabled_) {
            return false;
        }
        enabled_ = false;
        on_disable();
        return true;
    }

    bool set_enabled(bool requested) noexcept { return requested ? enable() : disable(); }

    // Lifecycle hooks. on_tick runs at the game's 20 Hz tick gate; on_render runs per frame and
    // must only draw. Returning false from on_enable refuses the enable (e.g. a game-state
    // prerequisite missing) and keeps the module off.
    virtual bool on_enable() noexcept { return true; }
    virtual void on_disable() noexcept = 0;
    virtual void on_tick(float /*delta_seconds*/) noexcept {}
    virtual void on_render(float /*delta_seconds*/) noexcept {}

protected:
    BaseModule(const char* name, const char* description, Category category, int bind) noexcept
        : name_(name), description_(description), category_(category), bind_(bind) {}

    // Called once by the concrete module's constructor, right after its settings members are
    // initialised: the array is built from member addresses, so it must run after them.
    template <std::size_t N>
    void register_settings(std::array<settings::Setting*, N> entries) noexcept {
        static_assert(N <= kMaxSettings, "a module cannot carry more than kMaxSettings");
        for (std::size_t index = 0; index < N; ++index) {
            setting_ptrs_[index] = entries[index];
        }
        setting_count_ = N;
    }

    static constexpr std::size_t kMaxSettings = 8;

private:
    const char* name_ = nullptr;
    const char* description_ = nullptr;
    Category category_ = Category::Misc;
    int bind_ = 0;
    bool enabled_ = false;

    // Mutable pointers behind a const module: settings are the module's configuration state, and
    // both the config engine and the GUI legitimately mutate them through a registry lookup.
    mutable std::array<settings::Setting*, kMaxSettings> setting_ptrs_{};
    std::size_t setting_count_ = 0;
};

} // namespace woke::modules
