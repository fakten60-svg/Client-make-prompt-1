#include "hooks/hook_manager.h"

#include <windows.h>

#include <MinHook.h>

#include <array>

#include "core/logger.h"

namespace woke::hooks {
namespace {

// The client hooks a handful of addresses (today two). A fixed registry keeps the manager
// allocation-free and gives shutdown a deterministic, reverse-order teardown list.
constexpr std::size_t kMaxHooks = 8;

std::array<Hook*, kMaxHooks> g_registry{};
std::size_t g_registry_size = 0;
bool g_engine_ready = false;

// MinHook's own MH_StatusToString exists only in newer releases, and the text matters when a
// hook fails inside someone else's game process, so the mapping lives here.
const char* status_text(MH_STATUS status) noexcept {
    switch (status) {
    case MH_OK: return "ok";
    case MH_ERROR_ALREADY_INITIALIZED: return "already initialized";
    case MH_ERROR_NOT_INITIALIZED: return "not initialized";
    case MH_ERROR_ALREADY_CREATED: return "already created";
    case MH_ERROR_NOT_CREATED: return "not created";
    case MH_ERROR_MEMORY_ALLOC: return "memory allocation failed";
    case MH_ERROR_MEMORY_PROTECT: return "memory protection change failed";
    case MH_ERROR_MODULE_NOT_FOUND: return "module not found";
    case MH_ERROR_FUNCTION_NOT_FOUND: return "function not found";
    default: return "unknown";
    }
}

void register_hook(Hook* hook) noexcept {
    if (g_registry_size < kMaxHooks) {
        g_registry[g_registry_size] = hook;
        ++g_registry_size;
    }
}

void unregister_hook(Hook* hook) noexcept {
    for (std::size_t index = 0; index < g_registry_size; ++index) {
        if (g_registry[index] != hook) {
            continue;
        }
        // Swap-remove: order does not matter for lookup, and it keeps the array dense.
        g_registry[index] = g_registry[g_registry_size - 1];
        g_registry[g_registry_size - 1] = nullptr;
        --g_registry_size;
        return;
    }
}

} // namespace

bool initialize() noexcept {
    if (g_engine_ready) {
        return true;
    }

    const MH_STATUS status = MH_Initialize();
    if (status == MH_ERROR_ALREADY_INITIALIZED) {
        // Another injected copy initialised the engine first. Sharing it is correct:
        // MinHook's state is process-wide and re-initialising would fail every later hook.
        WOKE_LOG_WARN("hook-manager: MinHook was already initialised - sharing the engine");
        g_engine_ready = true;
        return true;
    }
    if (status != MH_OK) {
        WOKE_LOG_ERROR("hook-manager: MH_Initialize failed (%s)", status_text(status));
        return false;
    }

    g_engine_ready = true;
    WOKE_LOG_INFO("hook-manager: MinHook ready (%s)", status_text(status));
    return true;
}

void shutdown() noexcept {
    // Reverse installation order, and each hook is disabled before it is removed. Removing a
    // hook whose trampoline can still be entered is the one reliable way to crash the render
    // thread, so disable-then-remove is not an optimisation here, it is the contract.
    for (std::size_t index = g_registry_size; index-- > 0;) {
        if (g_registry[index] != nullptr) {
            g_registry[index]->remove_now();
        }
    }
    g_registry_size = 0;

    if (g_engine_ready) {
        const MH_STATUS status = MH_Uninitialize();
        if (status != MH_OK) {
            WOKE_LOG_WARN("hook-manager: MH_Uninitialize failed (%s)", status_text(status));
        }
        g_engine_ready = false;
    }
}

Hook::~Hook() {
    remove_now();
}

bool Hook::install(const char* name, void* target, void* detour) noexcept {
    // The parameter shadows the nullary name() accessor inside this function, so every
    // diagnostic here goes through a local label instead of calling the accessor.
    const char* label = (name != nullptr) ? name : "(unnamed)";

    if (target_ != nullptr) {
        WOKE_LOG_WARN("hook-manager: '%s' is already installed", label);
        return false;
    }
    if (!g_engine_ready) {
        WOKE_LOG_ERROR("hook-manager: '%s' cannot be installed before initialize()", label);
        return false;
    }
    if (target == nullptr || detour == nullptr) {
        WOKE_LOG_ERROR("hook-manager: '%s' was given a null address", label);
        return false;
    }
    if (g_registry_size >= kMaxHooks) {
        WOKE_LOG_ERROR("hook-manager: the hook registry is full (%zu)", kMaxHooks);
        return false;
    }

    void* original = nullptr;
    const MH_STATUS status = MH_CreateHook(target, detour, &original);
    if (status != MH_OK) {
        WOKE_LOG_ERROR("hook-manager: creating '%s' at %p failed (%s)", label, target,
            status_text(status));
        return false;
    }

    name_ = label;
    target_ = target;
    detour_ = detour;
    original_ = original;
    enabled_ = false;
    removal_queued_ = false;
    register_hook(this);

    WOKE_LOG_INFO("hook-manager: '%s' armed at %p (trampoline %p)", name_, target_, original_);
    return true;
}

bool Hook::enable() noexcept {
    if (target_ == nullptr) {
        return false;
    }
    if (enabled_) {
        return true;
    }

    const MH_STATUS status = MH_EnableHook(target_);
    if (status != MH_OK) {
        WOKE_LOG_ERROR("hook-manager: enabling '%s' failed (%s)", name(), status_text(status));
        return false;
    }

    enabled_ = true;
    WOKE_LOG_INFO("hook-manager: '%s' enabled", name());
    return true;
}

bool Hook::disable() noexcept {
    if (target_ == nullptr || !enabled_) {
        return true;
    }

    const MH_STATUS status = MH_DisableHook(target_);
    if (status != MH_OK && status != MH_ERROR_NOT_CREATED) {
        WOKE_LOG_WARN("hook-manager: disabling '%s' failed (%s)", name(), status_text(status));
        return false;
    }

    enabled_ = false;
    WOKE_LOG_INFO("hook-manager: '%s' disabled", name());
    return true;
}

void Hook::queue_removal() noexcept {
    if (target_ == nullptr || removal_queued_) {
        return;
    }
    removal_queued_ = true;
    WOKE_LOG_INFO("hook-manager: '%s' queued for removal at the next safe point", name());
}

void Hook::remove_now() noexcept {
    if (target_ == nullptr) {
        return;
    }

    if (enabled_) {
        const MH_STATUS status = MH_DisableHook(target_);
        if (status != MH_OK && status != MH_ERROR_NOT_CREATED) {
            WOKE_LOG_WARN("hook-manager: disabling '%s' during removal failed (%s)", name(),
                status_text(status));
        }
        enabled_ = false;
    }

    const MH_STATUS status = MH_RemoveHook(target_);
    if (status != MH_OK && status != MH_ERROR_NOT_CREATED) {
        WOKE_LOG_ERROR("hook-manager: removing '%s' failed (%s)", name(), status_text(status));
    } else {
        WOKE_LOG_INFO("hook-manager: '%s' removed", name());
    }

    // Unregister by identity, not by index: the registry may have shifted under us.
    unregister_hook(this);

    name_ = nullptr;
    target_ = nullptr;
    detour_ = nullptr;
    original_ = nullptr;
    removal_queued_ = false;
}

void service_removals() noexcept {
    // Collect first, then remove: remove_now() mutates the registry, so walking it while
    // removing would need index gymnastics for no gain at this size.
    Hook* pending[kMaxHooks] = {};
    std::size_t pending_count = 0;

    for (std::size_t index = 0; index < g_registry_size; ++index) {
        Hook* hook = g_registry[index];
        if (hook != nullptr && hook->removal_queued() && hook->installed()) {
            pending[pending_count] = hook;
            ++pending_count;
        }
    }

    for (std::size_t index = 0; index < pending_count; ++index) {
        pending[index]->remove_now();
    }
}

std::size_t active_hook_count() noexcept {
    return g_registry_size;
}

std::size_t queued_removal_count() noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < g_registry_size; ++index) {
        if (g_registry[index] != nullptr && g_registry[index]->removal_queued()) {
            ++count;
        }
    }
    return count;
}

bool initialized() noexcept {
    return g_engine_ready;
}

} // namespace woke::hooks
