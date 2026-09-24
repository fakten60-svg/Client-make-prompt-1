#pragma once

// Windows-only helpers: paths, console plumbing and module resolution. Portable code
// (string_buffer.h, time_format.*) must never include this header - that is what keeps
// the host-side tests runnable on a non-Windows CI runner.
#ifndef _WIN32
#error "win32_utils.h is Windows-only; portable translation units must not include it."
#endif

#include <windows.h>
#include <cstddef>
#include <string>

namespace woke::util {

// Directory of the running executable. Inside javaw.exe this is the game directory,
// which is where logs/ and configs/ belong.
std::wstring executable_directory();

// Resolves a runtime asset (mappings.json, logs, configs) against the executable
// directory first and the current working directory second, so the client behaves the
// same whether it is injected by absolute path or launched from elsewhere.
// Returns a path with a trailing separator when the leaf is a directory name.
std::wstring resolve_runtime_path(const wchar_t* leaf);

// Creates a directory if missing. Returns true when the directory exists afterwards.
bool ensure_directory(const std::wstring& path);

// Reads a whole UTF-8 text file into memory (runtime assets such as mappings.json).
// Returns an empty string when the file cannot be opened or is larger than max_bytes, so
// the caller reports one concrete failure instead of acting on half-parsed content.
std::string read_text_file(const std::wstring& path, std::size_t max_bytes = 64u * 1024u * 1024u);

// Attaches to the parent console when the game was started from a terminal, otherwise
// allocates a fresh one. javaw.exe has no console, so this is what makes the colorized
// terminal output visible during local testing.
bool attach_console();

// Enables ANSI escape sequence processing on stdout/stderr (VT mode).
void enable_virtual_terminal();

// Unbuffered console write. Silently no-ops when no console is attached.
void write_console(const char* text, std::size_t length);

// OutputDebugString fallback so diagnostics survive when no console can be created.
void write_debugger(const char* text, std::size_t length);

} // namespace woke::util
