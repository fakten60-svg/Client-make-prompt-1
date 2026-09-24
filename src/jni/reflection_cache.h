#pragma once

// Reflection cache - the only place in woke.wtf that is allowed to call FindClass,
// GetMethodID, GetStaticMethodID or GetFieldID (blueprint §5.2, enforced by the review
// checklist in §11).
//
// Every lookup is a hash hit plus a pointer read after the first call, and the resolved
// handle is stored in the registry entry itself, so a per-frame read costs no allocation
// and no string copy. Failures are remembered per entry, which means a stale mapping costs
// one failed JNI call per member for the whole session - never a warning per frame.

#ifndef _WIN32
#error "reflection_cache.h is Windows-only; portable translation units must not include it."
#endif

#include <jni.h>
#include <cstddef>
#include <string_view>

#include "jni/mappings.h"

namespace woke::jni {

// Binds the parsed registry. Handles are resolved lazily on first use, so a member that no
// module ever touches is never looked up.
bool initialize(Mappings& registry) noexcept;

// Releases every global ref this cache owns (classes and the java.lang loader bridge).
void shutdown() noexcept;

// Returns nullptr - never a bogus handle - when a class or member cannot be resolved.
[[nodiscard]] jclass class_of(std::string_view class_alias) noexcept;
[[nodiscard]] jmethodID method_of(std::string_view class_alias, std::string_view member) noexcept;
[[nodiscard]] jmethodID static_method_of(
    std::string_view class_alias, std::string_view member) noexcept;
[[nodiscard]] jfieldID field_of(std::string_view class_alias, std::string_view member) noexcept;

struct CacheStats {
    std::size_t classes = 0;
    std::size_t methods = 0;
    std::size_t fields = 0;
    std::size_t failures = 0;
};

[[nodiscard]] CacheStats stats() noexcept;

} // namespace woke::jni
