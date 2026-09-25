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
//
// The module's own toggle bind is registered as its *last* setting (roadmap step 8). That single
// decision is what makes a rebind persist: the config engine already walks settings, so "the key
// that toggles this" becomes an ordinary value with no per-module persistence code, and the
// Keybinds page is a filter over the same walk rather than a second source of truth.

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
    // Bind that toggles this module (0 = unbound). It is a persisted setting (registered last), so
    // the keybind chip's rebind survives a config save/reload and a DLL reinjection.
    [[nodiscard]] int bind() const noexcept { return bind_setting_.value(); }
    void set_bind(int virtual_key) noexcept { bind_setting_.set(virtual_key); }

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

    // Key dispatch (roadmap step 8). Both edges arrive, so a module can react to a press, a
    // release, or both. Returns true when the key belongs to this module, which is what the
    // dispatcher uses to mark the event consumed and stop the game from also seeing it.
    //
    // The default is the step-5 behaviour - press toggles - now expressed as a module hook
    // instead of hard-coded manager logic, so an action module (Panic) or a hold module can
    // replace it without the manager knowing it exists.
    [[nodiscard]] virtual bool on_key(int virtual_key, bool down) noexcept {
        if (virtual_key == 0 || virtual_key != bind()) {
            return false;
        }
        if (hold_to_activate()) {
            return down ? enable() : disable();
        }
        return down && set_enabled(!enabled());
    }

    // True when the bind only holds the module on while the key is held (ClickGUI's hold mode, a
    // future aim-assist paddle). Press-to-toggle is the default.
    [[nodiscard]] virtual bool hold_to_activate() const noexcept { return false; }

    // True for a module that *does* something instead of holding an on/off state (Panic, the
    // profile hotkeys' helper). The GUI draws no toggle for it and the keybind toast skips it,
    // because "Panic: off" is a sentence that should never reach the user.
    [[nodiscard]] virtual bool is_action() const noexcept { return false; }

protected:
    BaseModule(const char* name, const char* description, Category category, int bind) noexcept
        : name_(name), description_(description), category_(category),
          bind_setting_("bind", "Key that toggles this module", bind) {
        // The bind is present from construction, so a module that carries no settings of its own
        // (Panic) still gets a persisted one. register_settings() re-points slot 0..N-1 at the
        // module's own settings and leaves the bind last.
        setting_ptrs_[0] = &bind_setting_;
        setting_count_ = 1;
    }

    // Called once by the concrete module's constructor, right after its settings members are
    // initialised: the array is built from member addresses, so it must run after them. The
    // module's own bind is appended as the last setting (see the file comment), which is why the
    // bound is kMaxSettings - 1 rather than kMaxSettings.
    template <std::size_t N>
    void register_settings(std::array<settings::Setting*, N> entries) noexcept {
        static_assert(N + 1 <= kMaxSettings,
            "a module cannot carry more than kMaxSettings - 1 settings plus its bind");
        for (std::size_t index = 0; index < N; ++index) {
            setting_ptrs_[index] = entries[index];
        }
        setting_ptrs_[N] = &bind_setting_;
        setting_count_ = N + 1;
    }

    static constexpr std::size_t kMaxSettings = 8;

private:
    const char* name_ = nullptr;
    const char* description_ = nullptr;
    Category category_ = Category::Misc;
    bool enabled_ = false;

    // The module's toggle bind, as a setting. Declared after the plain fields so the constructor's
    // member-init list (name, description, category, bind) still runs in declaration order.
    settings::BindSetting bind_setting_;

    // Mutable pointers behind a const module: settings are the module's configuration state, and
    // both the config engine and the GUI legitimately mutate them through a registry lookup.
    mutable std::array<settings::Setting*, kMaxSettings> setting_ptrs_{};
    std::size_t setting_count_ = 0;
};

} // namespace woke::modules
