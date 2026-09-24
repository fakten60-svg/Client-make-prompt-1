#include "hooks/swap_hook.h"

#include <windows.h>

#include <atomic>

#include "core/logger.h"
#include "hooks/game_thread.h"
#include "hooks/hook_manager.h"
#include "ui/gui.h"

namespace woke::hooks {
namespace {

using SwapBuffersFn = BOOL(WINAPI*)(HDC);

Hook g_swap_hook;
SwapBuffersFn g_original_swap_buffers = nullptr;

// Atomic because the render thread writes it while the worker thread may report it during
// shutdown; relaxed ordering is enough for a plain counter.
std::atomic<std::uint64_t> g_frame_count{0};

// The detour runs once per frame on the render thread, so it is kept to three statements:
// service deferred removals, run the frame pipeline, call through to the original. Anything
// else belongs in game_thread::on_frame().
BOOL WINAPI swap_buffers_detour(HDC device_context) noexcept {
    // Safe point. This thread can be inside a detour of ours, and here it provably is not
    // inside another one, so a hook that queued its removal can be removed now. The swap hook
    // itself must never queue its own removal while enabled - it is removed explicitly after
    // being disabled (see remove_swap_hook).
    service_removals();

    g_frame_count.fetch_add(1, std::memory_order_relaxed);
    game_thread::on_frame();

    // Step 4: the overlay. render_overlay() is the suppression gate as well as the renderer - while
    // the chrome is hidden it returns before ImGui::NewFrame, so a suppressed frame does no ImGui
    // work at all (§7.8). It is called here, on the render thread, because this is the only point
    // in the process where the game's OpenGL context is guaranteed to be current.
    ui::render_overlay();

    if (g_original_swap_buffers == nullptr) {
        // Unreachable in practice (install() arms the trampoline only after the original
        // pointer is cached), but a null call through here would take the game down.
        return TRUE;
    }
    return g_original_swap_buffers(device_context);
}

void* resolve_swap_buffers() noexcept {
    // LWJGL loads opengl32.dll for us; the explicit LoadLibraryW is only here for the case
    // where the client is injected before the game has created its GL context.
    HMODULE opengl = ::GetModuleHandleW(L"opengl32.dll");
    if (opengl == nullptr) {
        opengl = ::LoadLibraryW(L"opengl32.dll");
    }
    if (opengl == nullptr) {
        WOKE_LOG_ERROR("swap-hook: opengl32.dll is not available in this process");
        return nullptr;
    }

#pragma warning(push)
#pragma warning(disable : 4191) // GetProcAddress hands back an address, not a typed pointer
    const auto address = reinterpret_cast<void*>(::GetProcAddress(opengl, "wglSwapBuffers"));
#pragma warning(pop)
    return address;
}

} // namespace

bool install_swap_hook() noexcept {
    if (g_swap_hook.installed()) {
        return true;
    }

    void* target = resolve_swap_buffers();
    if (target == nullptr) {
        WOKE_LOG_ERROR("swap-hook: wglSwapBuffers was not found - no frame boundary available");
        return false;
    }

    if (!g_swap_hook.install("wglSwapBuffers", target, detail::as_address(&swap_buffers_detour))) {
        return false;
    }

    // The original pointer is cached before the hook goes live, so the first detour call
    // already has somewhere to call through to.
    g_original_swap_buffers = g_swap_hook.original_as<SwapBuffersFn>();
    if (g_original_swap_buffers == nullptr) {
        WOKE_LOG_ERROR("swap-hook: MinHook did not return a trampoline for wglSwapBuffers");
        g_swap_hook.remove_now();
        return false;
    }

    if (!g_swap_hook.enable()) {
        g_swap_hook.remove_now();
        return false;
    }

    g_frame_count.store(0, std::memory_order_relaxed);
    WOKE_LOG_INFO("swap-hook: wglSwapBuffers hooked at %p (trampoline %p)", target,
        static_cast<void*>(g_swap_hook.original()));
    return true;
}

void remove_swap_hook() noexcept {
    if (!g_swap_hook.installed()) {
        return;
    }

    // Disable first, then remove: with the hook disabled no new frame can enter the detour,
    // which is what makes an immediate removal safe here.
    g_swap_hook.disable();
    g_swap_hook.remove_now();
    g_original_swap_buffers = nullptr;

    WOKE_LOG_INFO("swap-hook: removed after %llu frame(s)",
        static_cast<unsigned long long>(g_frame_count.load(std::memory_order_relaxed)));
}

bool swap_hook_installed() noexcept {
    return g_swap_hook.installed();
}

std::uint64_t swap_hook_frame_count() noexcept {
    return g_frame_count.load(std::memory_order_relaxed);
}

} // namespace woke::hooks
