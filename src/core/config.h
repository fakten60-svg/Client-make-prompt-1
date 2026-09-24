#pragma once

// Config persistence (blueprint §4.2).
//
// `configs/<name>.json`, one flat document: { "module": { "setting": value } }. The engine walks
// every registered module's settings, so a new module persists with zero config code (the §4.2
// auto-binding invariant). Tolerance contract: missing keys keep defaults, unknown module or
// setting keys are counted and reported, wrong-typed values are ignored, and nothing throws.
//
// File IO is an injectable interface rather than a direct Win32 call, which is what makes the
// persistence contract testable on the host test runner: save/load against an in-memory map is
// byte-for-byte the path the Windows implementation takes. The default backend (core/config's
// own) wraps util::read_text_file / a std::fwrite write through the win32 layer at runtime.

#include <cstddef>
#include <string>
#include <string_view>

namespace woke::config {

// Binary interface for config storage. Implementations must tolerate any path string.
class Storage {
public:
    virtual ~Storage() = default;
    [[nodiscard]] virtual bool read(std::string_view path, std::string& out) = 0;
    [[nodiscard]] virtual bool write(std::string_view path, std::string_view contents) = 0;
};

// Installs a custom backend (tests) and returns the previous one. nullptr restores the default
// file-backed store.
void set_storage(Storage* storage) noexcept;

struct LoadReport {
    std::size_t modules_matched = 0;
    std::size_t settings_applied = 0;
    std::size_t unknown_modules = 0;
    std::size_t unknown_settings = 0;
    std::size_t wrong_type = 0;
    bool parsed = false;

    [[nodiscard]] bool clean() const noexcept { return parsed && unknown_modules == 0; }
};

// Serializes every registered module's settings to JSON. Returns an empty string only when the
// registry is empty (saving nothing is not an error).
[[nodiscard]] std::string serialize() noexcept;

// Applies a config document to the live modules. Never throws; a malformed document is reported
// through LoadReport::parsed = false.
[[nodiscard]] LoadReport deserialize(std::string_view json_text) noexcept;

// Saves the live state to configs/<name>.json. Returns false when the write failed; the caller
// logs and toasts, but the session continues either way.
[[nodiscard]] bool save(std::string_view name) noexcept;

// Requests a save of the named config on the worker thread. This is the game thread's save path:
// §4.2 keeps config IO off the render path, so a GUI toggle or keybind asks instead of writing.
// The request is coalesced - asking twice before the worker services it writes the file once.
// `name` must be a short literal (fixed 64-byte mailbox).
void request_save(const char* name) noexcept;

// Services one pending save request. Called from the worker loop; returns true when a save ran.
[[nodiscard]] bool service_saves() noexcept;

// Loads configs/<name>.json into the live modules. A missing file keeps defaults and reports
// parsed = false with no applied settings - the first boot case, not an error.
[[nodiscard]] LoadReport load(std::string_view name) noexcept;

// The path the named config maps to ("configs/default.json"), for logs. Exposed because both
// call sites log it.
[[nodiscard]] std::string path_for(std::string_view name) noexcept;

} // namespace woke::config
