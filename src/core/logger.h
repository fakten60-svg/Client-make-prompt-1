#pragma once

#include <cstddef>
#include <cstdint>

namespace woke::logger {

enum class Level : std::uint8_t { Debug = 0, Info, Warn, Error };

// Absolute path of the active session log file, or an empty string when the logger has
// not been initialised. Used by the boot banner and by support reports.
const wchar_t* session_log_path() noexcept;

// Creates <game dir>/logs, opens the date-and-time-stamped session file plus the
// latest.log mirror, prunes old sessions and prepares the colorized console.
// Returns false only when no log file could be opened; console logging still works.
bool init() noexcept;

// Flushes queued lines and closes both streams. Idempotent.
void shutdown() noexcept;

// Drains queued lines to disk. Called from the worker thread only: never blocks and
// never allocates, so it is safe to call at any frequency.
std::size_t flush() noexcept;

// Producer entry point: formats one line, writes the colored console line immediately
// and queues the file copy. Allocation-free and non-blocking by design, so modules are
// free to log from inside tick/render paths.
void write(Level level, const char* file, int line, const char* format, ...) noexcept;

// Lines dropped because the queue was saturated. Reported once at shutdown so a noisy
// session is visible instead of silently truncated.
std::size_t dropped_line_count() noexcept;

} // namespace woke::logger

#define WOKE_LOG_DEBUG(...) \
    ::woke::logger::write(::woke::logger::Level::Debug, __FILE__, __LINE__, __VA_ARGS__)
#define WOKE_LOG_INFO(...) \
    ::woke::logger::write(::woke::logger::Level::Info, __FILE__, __LINE__, __VA_ARGS__)
#define WOKE_LOG_WARN(...) \
    ::woke::logger::write(::woke::logger::Level::Warn, __FILE__, __LINE__, __VA_ARGS__)
#define WOKE_LOG_ERROR(...) \
    ::woke::logger::write(::woke::logger::Level::Error, __FILE__, __LINE__, __VA_ARGS__)
