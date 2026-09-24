#pragma once

#include <windows.h>

namespace woke::lifecycle {

// Boots every core system in a fixed, logged order. Runs on the DLL worker thread
// only: it never touches the loader lock and never touches the JVM, so a missing or
// broken dependency degrades to a console-only client instead of a deadlock.
void boot(HMODULE self) noexcept;

// Reverse of boot(): flushes logs and releases core resources.
void shutdown() noexcept;

// Worker loop: drains log writes and idles until an unload is requested. Kept here so
// dllmain owns nothing but the thread bootstrap.
void run_worker_loop() noexcept;

// Requests a clean unload (keybind / panic key in later steps, test hook today).
void request_unload() noexcept;
[[nodiscard]] bool unload_requested() noexcept;

} // namespace woke::lifecycle
