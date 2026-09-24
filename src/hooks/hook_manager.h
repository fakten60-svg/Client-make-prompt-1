#pragma once

// RAII wrapper over MinHook (blueprint §4.5).
//
// Two rules shape this file:
//
//   * install() only creates the hook; enable() is separate. That is what lets the caller
//     cache the trampoline's original pointer *before* the first detour call can happen -
//     an enabled hook whose original pointer is still null is a null dereference inside the
//     render loop.
//   * A hook can ask to be removed from inside its own trampoline without deadlocking
//     MinHook: queue_removal() only marks it, and service_removals() performs the removal at
//     a point the caller knows is safe (the top of the swap trampoline).
//
// Windows-only: it includes MinHook.h and is compiled into the DLL only.

#ifndef _WIN32
#error "hook_manager.h is Windows-only; portable translation units must not include it."
#endif

#include <cstddef>

namespace woke::hooks {

// Arms MinHook's trampoline engine. Idempotent, and tolerant of another injected instance
// having initialised it first.
bool initialize() noexcept;

// Removes every hook this manager owns, in reverse installation order, then shuts the
// engine down. Safe to call when nothing was installed.
void shutdown() noexcept;

class Hook {
public:
    Hook() = default;
    ~Hook();

    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;

    // Creates the hook. It does NOT take effect until enable() returns true.
    bool install(const char* name, void* target, void* detour) noexcept;
    bool enable() noexcept;
    bool disable() noexcept;

    // Deferred removal: safe from inside the detour, performed by service_removals().
    void queue_removal() noexcept;

    // Immediate removal. Only safe when nothing can be executing this hook's trampoline.
    void remove_now() noexcept;

    [[nodiscard]] const char* name() const noexcept { return name_ != nullptr ? name_ : "(none)"; }
    [[nodiscard]] void* target() const noexcept { return target_; }
    [[nodiscard]] void* original() const noexcept { return original_; }
    [[nodiscard]] bool installed() const noexcept { return target_ != nullptr; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool removal_queued() const noexcept { return removal_queued_; }

    // MinHook hands back the trampoline as void*, and every caller needs it typed. This is
    // the one place that performs the data-pointer to function-pointer conversion, so the
    // two warnings that conversion legitimately raises are silenced here and nowhere else.
    template <typename Fn>
    [[nodiscard]] Fn original_as() const noexcept {
#pragma warning(push)
#pragma warning(disable : 4055 4191)
        return reinterpret_cast<Fn>(original_);
#pragma warning(pop)
    }

private:
    const char* name_ = nullptr;
    void* target_ = nullptr;
    void* detour_ = nullptr;
    void* original_ = nullptr;
    bool enabled_ = false;
    bool removal_queued_ = false;
};

namespace detail {

// Function pointer to address, for the MinHook entry points that take LPVOID.
template <typename Fn>
[[nodiscard]] void* as_address(Fn function) noexcept {
#pragma warning(push)
#pragma warning(disable : 4055 4191)
    return reinterpret_cast<void*>(function);
#pragma warning(pop)
}

} // namespace detail

// Performs the removals queued since the last call. Call from a known-safe point on the
// thread that can be inside a trampoline - in practice the top of the swap trampoline.
void service_removals() noexcept;

[[nodiscard]] std::size_t active_hook_count() noexcept;
[[nodiscard]] std::size_t queued_removal_count() noexcept;
[[nodiscard]] bool initialized() noexcept;

} // namespace woke::hooks
