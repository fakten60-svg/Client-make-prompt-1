#include "core/config.h"

#if defined(_WIN32)
#include <direct.h> // _mkdir
#else
#include <sys/stat.h> // mkdir
#endif

#include <cstdio>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>

#include <exception>

#include <nlohmann/json.hpp>

#include "modules/module_manager.h"

namespace woke::config {
namespace {

using nlohmann::json;

// The default store: plain stdio. The DLL links the static CRT and never passes CRT objects
// across its own boundary, so stdio here is safe inside the JVM process. Paths handed in are
// UTF-8 relative paths ("configs/<name>.json") resolved by the caller on Windows.
class FileStorage final : public Storage {
public:
    bool read(std::string_view path, std::string& out) override {
        // Secure CRT variant on Windows: MSVC deprecates plain fopen (C4996) and the project
        // builds with warnings as errors; the logger's file layer follows the same rule.
#if defined(_WIN32)
        std::FILE* handle = nullptr;
        if (::fopen_s(&handle, std::string(path).c_str(), "rb") != 0) {
            return false;
        }
#else
        std::FILE* handle = std::fopen(std::string(path).c_str(), "rb");
        if (handle == nullptr) {
            return false;
        }
#endif
        char buffer[4096];
        std::size_t chunk = 0;
        while ((chunk = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) {
            out.append(buffer, chunk);
            if (out.size() > kMaxConfigBytes) {
                (void)std::fclose(handle);
                return false; // a config larger than this is corrupt by definition
            }
        }
        const bool ok = std::ferror(handle) == 0;
        (void)std::fclose(handle);
        return ok;
    }

    bool write(std::string_view path, std::string_view contents) override {
        // Create the parent directory on the first save ("configs/" does not exist yet). Only the
        // first path component is created because configs/ is always exactly one level deep.
        const std::size_t slash = path.find('/');
        if (slash != std::string_view::npos) {
        // Create the parent directory on the first save ("configs/" does not exist yet).
#if defined(_WIN32)
            (void)::_mkdir(std::string(path.substr(0, slash)).c_str());
#else
            (void)::mkdir(std::string(path.substr(0, slash)).c_str(), 0755);
#endif
        }
        // QA F2: the write lands in a sibling temp file first and is renamed over the target,
        // so a crash mid-write leaves the previous config intact instead of a truncated file.
        // The rename is best-effort because std::rename cannot replace an existing file on all
        // Windows runtimes; the target is removed first, which keeps the window where no config
        // exists down to one call instead of exposing a half-written one for the whole write.
        const std::string target_path(path);
        const std::string temp_path = target_path + ".tmp";
        std::FILE* handle = nullptr;
#if defined(_WIN32)
        if (::fopen_s(&handle, temp_path.c_str(), "wb") != 0) {
            return false;
        }
#else
        handle = std::fopen(temp_path.c_str(), "wb");
        if (handle == nullptr) {
            return false;
        }
#endif
        const std::size_t written = std::fwrite(contents.data(), 1, contents.size(), handle);
        if (written != contents.size() || std::fclose(handle) != 0) {
            (void)std::remove(temp_path.c_str());
            return false;
        }
        // std::remove/std::rename take C strings, and a string_view is not guaranteed to be
        // NUL-terminated, so both paths go through the owning std::string.
        (void)std::remove(target_path.c_str());
        if (std::rename(temp_path.c_str(), target_path.c_str()) != 0) {
            (void)std::remove(temp_path.c_str());
            return false;
        }
        return true;
    }

private:
    // One config with 24 modules x 8 settings is a few KB; anything two orders larger is corrupt.
    static constexpr std::size_t kMaxConfigBytes =
        static_cast<std::size_t>(256u) * static_cast<std::size_t>(1024u);
};

Storage* g_storage = nullptr; // nullptr = default file store

// The deferred-save mailbox (§4.2). The game thread writes a request; the worker thread services
// it. One slot is enough because requests coalesce: a second request before the worker wakes
// overwrites the first, and the file it eventually writes is at least as new.
constexpr std::size_t kSaveNameCapacity = 64;
std::atomic<bool> g_save_pending{false};
char g_save_name[kSaveNameCapacity] = {};

// The deferred-load mailbox (step 8). Same shape as the save one and for the same reason, but
// serviced on the game thread: a load mutates module state, so it must not run concurrently with
// the tick/key fan-out. Only the *when* differs, not the coalescing rule.
std::atomic<bool> g_load_pending{false};
char g_load_name[kSaveNameCapacity] = {};

// Truncating copy into a fixed mailbox: names come from call-site literals, so a clipped name is a
// visible bug (wrong file) rather than a crash.
void park_name(char (&mailbox)[kSaveNameCapacity], const char* name) noexcept {
    if (name == nullptr) {
        mailbox[0] = '\0';
        return;
    }
    for (std::size_t index = 0; index < kSaveNameCapacity - 1; ++index) {
        mailbox[index] = name[index];
        if (name[index] == '\0') {
            break;
        }
    }
    mailbox[kSaveNameCapacity - 1] = '\0';
}

// Serialises "consume the pending flag" with "copy the name out of the mailbox" (QA F1). The
// consumer must observe the name that belongs to the flag it took: reading the shared mailbox
// after exchange() let a request posted between the two steps tear the name and write one
// config's contents into another slot's file.
//
// A spinlock rather than std::mutex: the critical section is one bounded copy (nanoseconds),
// the file stays free of <mutex> (some libstdc++ packagings - the mingw-w64 win32-threads
// build - ship none of it), and there is no allocation on either path.
std::atomic_flag g_save_name_lock = ATOMIC_FLAG_INIT;
std::atomic_flag g_load_name_lock = ATOMIC_FLAG_INIT;

// Copies a name out of its mailbox under the mailbox's spinlock.
void take_name(std::atomic_flag& guard, const char (&mailbox)[kSaveNameCapacity],
    char* out) noexcept {
    while (guard.test_and_set(std::memory_order_acquire)) {
        // spin: the holder is copying a bounded name and leaves immediately
    }
    for (std::size_t index = 0; index < kSaveNameCapacity; ++index) {
        out[index] = mailbox[index];
        if (mailbox[index] == '\0') {
            break;
        }
    }
    out[kSaveNameCapacity - 1] = '\0';
    guard.clear(std::memory_order_release);
}

Storage& store() noexcept {
    static FileStorage default_store;
    return g_storage != nullptr ? *g_storage : default_store;
}

void collect_modules(json& out) noexcept {
    // json::operator[] inserts and the assignment can reallocate, both of which throw. The
    // noexcept is the contract the boot path relies on, so the throwing part is fenced here
    // rather than removed from the signature (QA: bugprone-exception-escape).
    try {
        const modules::ModuleManager& registry = modules::manager();
        for (std::size_t index = 0; index < registry.count(); ++index) {
            const modules::BaseModule* module = registry.at(index);
            json& module_node = out[module->name()];
            for (std::size_t entry = 0; entry < module->setting_count(); ++entry) {
                settings::Setting* setting =
                    const_cast<settings::Setting*>(module->settings()[entry]);
                json value;
                setting->to_json(value);
                if (!value.is_null()) {
                    module_node[setting->name()] = std::move(value);
                }
            }
            // The enabled state is part of the session the user expects to come back (§4.4:
            // enable / disable is the only mutation path, and it is what a config snapshot is
            // *of*).
            module_node["enabled"] = module->enabled();
        }
    } catch (...) {
        // A partial document is worse than an empty one: drop it and let save() refuse.
        out = json::object();
    }
}

} // namespace

// The throwing body of deserialize(), fenced by the noexcept wrapper above.
LoadReport deserialize_guarded(std::string_view json_text, LoadReport& report);

void set_storage(Storage* storage) noexcept {
    g_storage = storage;
}

std::string serialize() noexcept {
    try {
        json document = json::object();
        collect_modules(document);
        return document.dump(1);
    } catch (...) {
        return {};
    }
}

LoadReport deserialize(std::string_view json_text) noexcept {
    LoadReport report;
    try {
        return deserialize_guarded(json_text, report);
    } catch (...) {
        // The guard already ran; the report says how far the load got before the exception.
        return report;
    }
}

LoadReport deserialize_guarded(std::string_view json_text, LoadReport& report) {
    // Loaded state counts as a mutation: the GUI's badges and the enabled fan-out must reflect it.
    struct BucketRefresh {
        modules::ModuleManager& registry;
        ~BucketRefresh() noexcept { registry.notify_changed(); }
    } refresh{modules::manager()};
    if (json_text.empty()) {
        return report;
    }

    json document;
    try {
        document = json::parse(json_text);
    } catch (...) {
        return report; // parsed stays false
    }
    if (!document.is_object()) {
        return report;
    }
    report.parsed = true;

    const modules::ModuleManager& registry = modules::manager();
    for (auto module_it = document.begin(); module_it != document.end(); ++module_it) {
        modules::BaseModule* module = registry.find(module_it.key());
        if (module == nullptr) {
            ++report.unknown_modules;
            continue;
        }
        if (!module_it.value().is_object()) {
            ++report.wrong_type;
            continue;
        }
        ++report.modules_matched;
        const json& module_node = module_it.value();
        for (auto setting_it = module_node.begin(); setting_it != module_node.end(); ++setting_it) {
            // "enabled" is the module's own key, not a setting name.
            if (setting_it.key() == "enabled") {
                if (setting_it.value().is_boolean()) {
                    if (module->set_enabled(setting_it.value().get<bool>())) {
                        ++report.settings_applied;
                    } else if (module->enabled() != setting_it.value().get<bool>()) {
                        // The module refused (its on_enable said no); that is a legitimate
                        // outcome, not a config problem.
                    }
                } else {
                    ++report.wrong_type;
                }
                continue;
            }

            bool matched = false;
            for (std::size_t entry = 0; entry < module->setting_count(); ++entry) {
                settings::Setting* setting =
                    const_cast<settings::Setting*>(module->settings()[entry]);
                if (setting_it.key() == setting->name()) {
                    matched = true;
                    if (setting->from_json(setting_it.value())) {
                        ++report.settings_applied;
                    } else {
                        ++report.wrong_type;
                    }
                    break;
                }
            }
            if (!matched) {
                ++report.unknown_settings;
            }
        }
    }
    return report;
}

std::string path_for(std::string_view name) noexcept {
    std::string path = "configs/";
    path.append(name);
    path.append(".json");
    return path;
}

void request_save(const char* name) noexcept {
    if (name == nullptr) {
        return;
    }
    while (g_save_name_lock.test_and_set(std::memory_order_acquire)) {
    }
    park_name(g_save_name, name);
    g_save_name_lock.clear(std::memory_order_release);
    g_save_pending.store(true, std::memory_order_release);
}

bool service_saves() noexcept {
    const bool pending = g_save_pending.exchange(false, std::memory_order_acq_rel);
    if (!pending) {
        return false;
    }
    try {
        // Deliberately no logger call: this file stays portable (no windows.h, no logger) so the
        // host tests can run it on Linux. The worker loop logs the save outcome through the
        // return value, and a false write is reported there - the same warnings-as-strings
        // discipline the mappings registry uses.
        char name[kSaveNameCapacity];
        take_name(g_save_name_lock, g_save_name, name);
        return save(name);
    } catch (...) {
        // A refused save beats an escape through the noexcept boundary.
        return false;
    }
}

void request_load(const char* name) noexcept {
    if (name == nullptr) {
        return;
    }
    while (g_load_name_lock.test_and_set(std::memory_order_acquire)) {
    }
    park_name(g_load_name, name);
    g_load_name_lock.clear(std::memory_order_release);
    g_load_pending.store(true, std::memory_order_release);
}

bool service_loads(LoadReport& out) noexcept {
    const bool pending = g_load_pending.exchange(false, std::memory_order_acq_rel);
    if (!pending) {
        return false;
    }
    char name[kSaveNameCapacity];
    take_name(g_load_name_lock, g_load_name, name);
    out = load(name);
    return true;
}

bool save(std::string_view name) noexcept {
    try {
        const std::string contents = serialize();
        if (contents.empty()) {
            return false;
        }
        // The configs/ directory may not exist on the first boot; the store creates what it can.
        return store().write(path_for(name), contents);
    } catch (...) {
        // Allocation failure during serialization or path building is a refused save, never an
        // escape through a noexcept boundary on the worker thread.
        return false;
    }
}

LoadReport load(std::string_view name) noexcept {
    try {
        std::string contents;
        if (!store().read(path_for(name), contents)) {
            return LoadReport{}; // first boot: defaults stay, nothing is an error
        }
        return deserialize(contents);
    } catch (...) {
        return LoadReport{}; // unreadable config means defaults, exactly like a missing one
    }
}

} // namespace woke::config
