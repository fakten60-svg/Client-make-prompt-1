#pragma once

// mappings.json registry - the single source of every JNI identifier the client uses.
//
// This file is intentionally free of jni.h, windows.h and the logger, which has two
// consequences worth stating plainly:
//
//   1. It runs on any host, so the parser (including the injected-string handling and
//      every documented schema variant, §5.4 of the blueprint) is covered by the host
//      test suite instead of only by an in-game session.
//   2. It never logs. Problems are collected as strings and the caller decides how to
//      report them, so a failed parse cannot depend on a logger that may not be up yet.
//
// Intermediary member names are not always method_XXXX/field_XXXX: recent Yarn builds
// also emit comp_XXXX for record components, so nothing here may assume a name prefix.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace woke::jni {

enum class MemberKind : std::uint8_t { Method, Field };

// Transparent-string hash.
//
// std::hash<std::string_view> only advertises is_transparent on newer standard libraries,
// so the registry brings its own hasher instead of depending on the toolchain's version of
// the standard library. That is what lets a std::string_view be looked up directly - the
// property that keeps a per-tick JNI read free of a temporary std::string.
struct StringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept {
        return std::hash<std::string_view>{}(text);
    }
    [[nodiscard]] std::size_t operator()(const std::string& text) const noexcept {
        return std::hash<std::string_view>{}(text);
    }
    [[nodiscard]] std::size_t operator()(const char* text) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(text));
    }
};

template <typename Value>
using StringMap = std::unordered_map<std::string, Value, StringHash, std::equal_to<>>;

using StringKeyMap = StringMap<std::string>;

// One resolved member (method or field) of one class.
struct MemberEntry {
    std::string yarn_name;    // "getInstance" - the key modules and the cache look up by
    std::string intermediary; // "method_1551" - the real runtime identifier
    std::string descriptor;   // "()Lnet/minecraft/class_310;" - intermediary-encoded

    // Opaque JNI handle slots, written exactly once by reflection_cache. They are void*
    // rather than jclass/jmethodID/jfieldID so this header stays JNI-free and testable.
    void* handle = nullptr;        // jmethodID (instance) or jfieldID
    void* static_handle = nullptr; // jmethodID when the member is a static method
    bool handle_failed = false;    // already failed once: never retry, never spam JNI
    bool static_handle_failed = false;
};

// One mapped class plus every alias it can be addressed by.
struct ClassEntry {
    std::string yarn_path;           // net/minecraft/client/MinecraftClient
    std::string intermediary_path;   // net/minecraft/class_310
    std::string simple_name;         // MinecraftClient
    std::string intermediary_simple; // class_310

    // std::string_view keys hit these maps without allocating (§6.3).
    StringMap<MemberEntry> methods;
    StringMap<MemberEntry> fields;

    // Intermediary name -> Yarn name. A member stays a single canonical entry, and looking
    // it up by its obfuscated identifier ("method_1551") still finds that one entry.
    StringKeyMap method_ids;
    StringKeyMap field_ids;

    void* java_class = nullptr; // jclass global ref, owned by reflection_cache
    bool class_failed = false;
};

// Result of one boot-time anchor assertion (§5.4).
struct AnchorCheck {
    std::string class_alias;
    std::string member; // empty for a class-only anchor
    MemberKind kind = MemberKind::Method;
    std::string expected_intermediary;
    bool resolved = false;
    bool matches_expected = false;
};

struct AnchorStatus {
    std::vector<AnchorCheck> checks;
    std::size_t resolved = 0;
    std::size_t failed = 0;
    std::size_t mismatched = 0;

    [[nodiscard]] bool all_ok() const noexcept { return failed == 0 && mismatched == 0; }
};

class Mappings {
public:
    static constexpr const char* kAssetName = "mappings.json";

    // Parses an in-memory JSON document. Tolerant by contract: a malformed document, a
    // missing "classes" section or a nonsense entry yields false (or a warning entry)
    // instead of an exception, because a broken asset must never take the game down.
    bool parse(std::string_view json_text);

    // Reads a UTF-8 file then parses it. Used by the host tests; on Windows the caller
    // resolves a wide path and reads through win32_utils so non-ASCII install paths work.
    bool load_file(const char* path);

    void reset();

    [[nodiscard]] bool loaded() const noexcept { return !classes_.empty(); }

    // Lookup by any alias: yarn simple name, intermediate simple name, intermediary path,
    // yarn path, or an explicit alias from the asset.
    [[nodiscard]] const ClassEntry* find_class(std::string_view alias) const noexcept;
    [[nodiscard]] const MemberEntry* find_member(
        std::string_view class_alias, std::string_view member, MemberKind kind) const noexcept;
    [[nodiscard]] const MemberEntry* find_method(
        std::string_view class_alias, std::string_view member) const noexcept;
    [[nodiscard]] const MemberEntry* find_field(
        std::string_view class_alias, std::string_view member) const noexcept;

    // Verifies that the identifiers the core depends on actually resolved, and that each
    // one still points at the identifier the blueprint documents. A silent rename upstream
    // is the failure mode this catches.
    [[nodiscard]] AnchorStatus verify_anchors() const;

    // Visits every canonical class entry (aliases are not visited twice). reflection_cache
    // uses this to release the global refs it owns in mirror image of how it created them.
    template <typename Visitor>
    void for_each_class(Visitor&& visitor) {
        for (auto& pair : classes_) {
            visitor(pair.second);
        }
    }

    [[nodiscard]] std::size_t class_count() const noexcept { return classes_.size(); }
    [[nodiscard]] std::size_t method_count() const noexcept { return method_count_; }
    [[nodiscard]] std::size_t field_count() const noexcept { return field_count_; }
    [[nodiscard]] const std::string& version() const noexcept { return version_; }
    [[nodiscard]] const std::string& source() const noexcept { return source_; }
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return warnings_; }
    [[nodiscard]] bool warnings_truncated() const noexcept { return warnings_truncated_; }

private:
    void warn(std::string message);

    StringMap<ClassEntry> classes_;
    StringMap<ClassEntry*> aliases_;

    std::vector<std::string> warnings_;
    bool warnings_truncated_ = false;
    std::size_t method_count_ = 0;
    std::size_t field_count_ = 0;
    std::string version_;
    std::string source_;
};

} // namespace woke::jni
