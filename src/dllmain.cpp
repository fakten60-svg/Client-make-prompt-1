// woke.wtf - DLL entry point.
//
// Windows holds the loader lock while DLL_PROCESS_ATTACH runs. Doing anything
// non-trivial here (loading libraries, talking to the JVM, creating windows) is how
// injected clients deadlock the host process, so this file does exactly one thing:
// start a worker thread and return immediately. Every subsystem is initialised from
// that thread through lifecycle::boot().

#include <windows.h>

#include "core/lifecycle.h"

namespace {

HMODULE g_self = nullptr;

DWORD WINAPI worker_main(LPVOID /*parameter*/) noexcept {
    woke::lifecycle::boot(g_self);

    woke::lifecycle::run_worker_loop();

    woke::lifecycle::shutdown();

    // Clean self-eject: no thread is left behind inside the game process. This call is
    // declared noreturn, so there is deliberately no code after it.
    ::FreeLibraryAndExitThread(g_self, 0);
}

} // namespace

extern "C" __declspec(dllexport) void woke_request_unload() {
    // Small exported surface so a local test harness can exercise the full unload path
    // (teardown must be as carefully verified as boot).
    woke::lifecycle::request_unload();
}

extern "C" __declspec(dllexport) int woke_abi_version() {
    // Lets the injector refuse to load a module built for a different contract.
    return 1;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        ::DisableThreadLibraryCalls(module);

        const HANDLE worker = ::CreateThread(nullptr, 0, worker_main, nullptr, 0, nullptr);
        if (worker != nullptr) {
            ::CloseHandle(worker);
        }
    }
    return TRUE;
}
