#include "jni/reflection_cache.h"

#include "core/logger.h"
#include "jni/jni_context.h"
#include "utils/string_buffer.h"

namespace woke::jni {
namespace {

// Guards against one stale mapping turning into a per-frame log flood: the first failures
// are warnings, everything after that is debug-only.
constexpr std::size_t kMaxLookupWarnings = 16;

Mappings* g_registry = nullptr;
CacheStats g_stats{};
std::size_t g_lookup_warnings = 0;

// java.lang bridge used only for the classloader fallback below. Every handle here is a
// global ref owned by this file and released in shutdown().
jclass g_thread_class = nullptr;
jmethodID g_thread_current = nullptr;
jmethodID g_thread_get_context_loader = nullptr;
jmethodID g_thread_enumerate = nullptr;
jclass g_class_loader_class = nullptr;
jmethodID g_class_loader_load_class = nullptr;
jobject g_context_loader = nullptr;
bool g_loader_bridge_failed = false;

// java.lang.Double bridge for the boxed option values (game gamma / field of view). Owned here
// like every other class handle and released by unbind_registry().
jclass g_double_class = nullptr;
jmethodID g_double_value = nullptr;   // doubleValue()D
jmethodID g_double_value_of = nullptr; // valueOf(D)Ljava/lang/Double;

void report_lookup_failure(const char* what, const util::FixedString<128>& qualified) {
    if (g_lookup_warnings < kMaxLookupWarnings) {
        ++g_lookup_warnings;
        WOKE_LOG_WARN("jni: no mapping for %s '%s' - regenerate mappings.json", what,
            qualified.c_str());
        return;
    }
    WOKE_LOG_DEBUG("jni: no mapping for %s '%s'", what, qualified.c_str());
}

void report_missing_member(const char* what, std::string_view owner, std::string_view member) {
    util::FixedString<128> qualified;
    qualified.assign(owner);
    qualified.append(".");
    qualified.append(member);
    report_lookup_failure(what, qualified);
}

void report_missing_class(std::string_view class_alias) {
    util::FixedString<128> qualified;
    qualified.assign(class_alias);
    report_lookup_failure("class", qualified);
}

// Resolves the game's class loader.
//
// FindClass uses the calling thread's loader, and Minecraft's classes live in Knot's
// loader rather than the system one - so a native thread that attaches later cannot see
// them. The loader is therefore taken from a Java thread's context class loader: this
// thread's first, and then any live Java thread's, because a thread we created ourselves
// inherits no useful context loader from the JVM.
bool ensure_loader_bridge(JNIEnv* env) {
    if (g_loader_bridge_failed) {
        return false;
    }

    if (g_thread_class == nullptr) {
        jclass local_thread = env->FindClass("java/lang/Thread");
        if (local_thread == nullptr) {
            env->ExceptionClear();
            g_loader_bridge_failed = true;
            WOKE_LOG_ERROR("jni: java.lang.Thread is not resolvable - loader bridge disabled");
            return false;
        }

        g_thread_class = static_cast<jclass>(env->NewGlobalRef(local_thread));
        g_thread_current =
            env->GetStaticMethodID(local_thread, "currentThread", "()Ljava/lang/Thread;");
        g_thread_get_context_loader =
            env->GetMethodID(local_thread, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
        g_thread_enumerate =
            env->GetStaticMethodID(local_thread, "enumerate", "([Ljava/lang/Thread;)I");
        env->DeleteLocalRef(local_thread);

        if (g_thread_class == nullptr || g_thread_current == nullptr
            || g_thread_get_context_loader == nullptr) {
            env->ExceptionClear();
            g_loader_bridge_failed = true;
            WOKE_LOG_ERROR("jni: java.lang.Thread is missing expected members");
            return false;
        }
    }

    if (g_context_loader == nullptr) {
        jobject thread = env->CallStaticObjectMethod(g_thread_class, g_thread_current);
        if (thread != nullptr) {
            jobject loader = env->CallObjectMethod(thread, g_thread_get_context_loader);
            if (loader != nullptr) {
                g_context_loader = env->NewGlobalRef(loader);
                env->DeleteLocalRef(loader);
            }
            env->DeleteLocalRef(thread);
        }

        if (g_context_loader == nullptr && g_thread_enumerate != nullptr) {
            jobjectArray threads = env->NewObjectArray(16, g_thread_class, nullptr);
            if (threads != nullptr) {
                const jint count =
                    env->CallStaticIntMethod(g_thread_class, g_thread_enumerate, threads);
                for (jint index = 0; index < count && g_context_loader == nullptr; ++index) {
                    jobject candidate = env->GetObjectArrayElement(threads, index);
                    if (candidate == nullptr) {
                        continue;
                    }
                    jobject loader = env->CallObjectMethod(candidate, g_thread_get_context_loader);
                    if (loader != nullptr) {
                        g_context_loader = env->NewGlobalRef(loader);
                        env->DeleteLocalRef(loader);
                    }
                    env->DeleteLocalRef(candidate);
                }
                env->DeleteLocalRef(threads);
            }
        }

        if (env->ExceptionCheck() != JNI_FALSE) {
            env->ExceptionClear();
        }
        if (g_context_loader == nullptr) {
            g_loader_bridge_failed = true;
            WOKE_LOG_WARN("jni: no game class loader found - only system classes are reachable");
            return false;
        }
        WOKE_LOG_DEBUG("jni: game class loader acquired");
    }

    if (g_class_loader_load_class == nullptr) {
        jclass local_loader = env->FindClass("java/lang/ClassLoader");
        if (local_loader == nullptr) {
            env->ExceptionClear();
            g_loader_bridge_failed = true;
            WOKE_LOG_ERROR("jni: java.lang.ClassLoader is not resolvable");
            return false;
        }

        g_class_loader_class = static_cast<jclass>(env->NewGlobalRef(local_loader));
        g_class_loader_load_class = env->GetMethodID(
            local_loader, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        env->DeleteLocalRef(local_loader);

        if (g_class_loader_class == nullptr || g_class_loader_load_class == nullptr) {
            env->ExceptionClear();
            g_loader_bridge_failed = true;
            WOKE_LOG_ERROR("jni: java.lang.ClassLoader is missing loadClass");
            return false;
        }
    }

    return true;
}

// Returns a local reference the caller owns; it is released with the caller's local frame.
jclass load_class_with_loader(JNIEnv* env, const char* binary_name) {
    if (!ensure_loader_bridge(env)) {
        return nullptr;
    }

    jstring name = env->NewStringUTF(binary_name);
    if (name == nullptr) {
        return nullptr;
    }
    jclass result = static_cast<jclass>(
        env->CallObjectMethod(g_context_loader, g_class_loader_load_class, name));
    env->DeleteLocalRef(name);

    if (env->ExceptionCheck() != JNI_FALSE) {
        // ClassNotFoundException is an expected outcome for a stale mapping, not a bug.
        env->ExceptionClear();
        return nullptr;
    }
    return result;
}

// Resolves java.lang.Double once. A system class is reachable with a plain FindClass on a JVM
// thread; the loader fallback covers the case where this thread attached without a context
// loader. A failure is retried on the next call rather than cached, because the caller reports
// it as "the write could not be made" and a later attempt may well succeed.
bool ensure_double_bridge(JNIEnv* env) {
    if (g_double_class != nullptr && g_double_value != nullptr && g_double_value_of != nullptr) {
        return true;
    }

    jclass local_class = env->FindClass("java/lang/Double");
    if (local_class == nullptr) {
        env->ExceptionClear();
        local_class = load_class_with_loader(env, "java/lang/Double");
    }
    if (local_class == nullptr) {
        return false;
    }

    const jmethodID value = env->GetMethodID(local_class, "doubleValue", "()D");
    const jmethodID value_of =
        env->GetStaticMethodID(local_class, "valueOf", "(D)Ljava/lang/Double;");
    if (env->ExceptionCheck() != JNI_FALSE) {
        env->ExceptionClear();
    }
    if (value == nullptr || value_of == nullptr) {
        env->DeleteLocalRef(local_class);
        return false;
    }

    if (g_double_class != nullptr) {
        env->DeleteGlobalRef(g_double_class);
    }
    g_double_class = static_cast<jclass>(env->NewGlobalRef(local_class));
    env->DeleteLocalRef(local_class);
    if (g_double_class == nullptr) {
        return false;
    }
    g_double_value = value;
    g_double_value_of = value_of;
    return true;
}

jclass resolve_class(JNIEnv* env, ClassEntry& entry) {
    const char* binary_name = entry.intermediary_path.c_str();

    jclass local_class = env->FindClass(binary_name);
    if (local_class == nullptr) {
        env->ExceptionClear();
        local_class = load_class_with_loader(env, binary_name);
    }
    if (local_class == nullptr) {
        entry.class_failed = true;
        ++g_stats.failures;
        WOKE_LOG_WARN("jni: class '%s' did not resolve - is mappings.json current?", binary_name);
        return nullptr;
    }

    entry.java_class = env->NewGlobalRef(local_class);
    env->DeleteLocalRef(local_class);
    if (entry.java_class == nullptr) {
        entry.class_failed = true;
        ++g_stats.failures;
        WOKE_LOG_ERROR("jni: NewGlobalRef failed for '%s'", binary_name);
        return nullptr;
    }

    ++g_stats.classes;
    return static_cast<jclass>(entry.java_class);
}

// The registry stores open handles so it can stay JNI-free (mappings.h); this file is their
// only writer, so unwrapping them here is safe and intentional.
MemberEntry* writable(const MemberEntry* entry) {
    return const_cast<MemberEntry*>(entry);
}

bool descriptor_available(MemberEntry* entry) {
    if (!entry->descriptor.empty()) {
        return true;
    }
    // A plain-identifier member (schema variant 4) carries no signature, and a signature is
    // required to resolve a handle.
    entry->handle_failed = true;
    entry->static_handle_failed = true;
    ++g_stats.failures;
    WOKE_LOG_WARN("jni: mapping for '%s' (%s) has no descriptor - regenerate mappings.json",
        entry->yarn_name.c_str(), entry->intermediary.c_str());
    return false;
}

jmethodID resolve_method(
    std::string_view class_alias, std::string_view member, bool wants_static) {
    if (g_registry == nullptr) {
        return nullptr;
    }

    const MemberEntry* found = g_registry->find_method(class_alias, member);
    if (found == nullptr) {
        report_missing_member(wants_static ? "static method" : "method", class_alias, member);
        return nullptr;
    }

    MemberEntry* entry = writable(found);
    void*& slot = wants_static ? entry->static_handle : entry->handle;
    bool& failed = wants_static ? entry->static_handle_failed : entry->handle_failed;
    if (slot != nullptr) {
        return reinterpret_cast<jmethodID>(slot);
    }
    if (failed) {
        return nullptr;
    }
    if (!descriptor_available(entry)) {
        return nullptr;
    }

    JNIEnv* env = current_env();
    if (env == nullptr) {
        return nullptr; // not attached yet: retry later instead of caching a failure
    }
    jclass owner = class_of(class_alias);
    if (owner == nullptr) {
        failed = true;
        return nullptr; // class_of already reported and counted this
    }

    const jmethodID handle = wants_static
        ? env->GetStaticMethodID(owner, entry->intermediary.c_str(), entry->descriptor.c_str())
        : env->GetMethodID(owner, entry->intermediary.c_str(), entry->descriptor.c_str());
    if (handle == nullptr) {
        env->ExceptionClear();
        failed = true;
        ++g_stats.failures;
        WOKE_LOG_WARN("jni: method '%s' (%s%s) did not resolve", entry->yarn_name.c_str(),
            entry->intermediary.c_str(), entry->descriptor.c_str());
        return nullptr;
    }

    slot = reinterpret_cast<void*>(handle);
    ++g_stats.methods;
    return handle;
}

jfieldID resolve_field(std::string_view class_alias, std::string_view member) {
    if (g_registry == nullptr) {
        return nullptr;
    }

    const MemberEntry* found = g_registry->find_field(class_alias, member);
    if (found == nullptr) {
        report_missing_member("field", class_alias, member);
        return nullptr;
    }

    MemberEntry* entry = writable(found);
    if (entry->handle != nullptr) {
        return reinterpret_cast<jfieldID>(entry->handle);
    }
    if (entry->handle_failed) {
        return nullptr;
    }
    if (!descriptor_available(entry)) {
        return nullptr;
    }

    JNIEnv* env = current_env();
    if (env == nullptr) {
        return nullptr;
    }
    jclass owner = class_of(class_alias);
    if (owner == nullptr) {
        entry->handle_failed = true;
        return nullptr;
    }

    const jfieldID handle =
        env->GetFieldID(owner, entry->intermediary.c_str(), entry->descriptor.c_str());
    if (handle == nullptr) {
        env->ExceptionClear();
        entry->handle_failed = true;
        ++g_stats.failures;
        WOKE_LOG_WARN("jni: field '%s' (%s:%s) did not resolve", entry->yarn_name.c_str(),
            entry->intermediary.c_str(), entry->descriptor.c_str());
        return nullptr;
    }

    entry->handle = reinterpret_cast<void*>(handle);
    ++g_stats.fields;
    return handle;
}

} // namespace

void bind_registry(Mappings& registry) noexcept {
    g_registry = &registry;
    g_stats = CacheStats{};
    g_lookup_warnings = 0;
}

void unbind_registry() noexcept {
    JNIEnv* env = current_env();

    if (env != nullptr) {
        if (g_double_class != nullptr) {
            env->DeleteGlobalRef(g_double_class);
        }
        if (g_registry != nullptr) {
            g_registry->for_each_class([env](ClassEntry& entry) {
                if (entry.java_class != nullptr) {
                    env->DeleteGlobalRef(static_cast<jobject>(entry.java_class));
                    entry.java_class = nullptr;
                }
            });
        }
        if (g_context_loader != nullptr) {
            env->DeleteGlobalRef(g_context_loader);
        }
        if (g_class_loader_class != nullptr) {
            env->DeleteGlobalRef(g_class_loader_class);
        }
        if (g_thread_class != nullptr) {
            env->DeleteGlobalRef(g_thread_class);
        }
    }

    g_double_class = nullptr;
    g_double_value = nullptr;
    g_double_value_of = nullptr;
    g_context_loader = nullptr;
    g_class_loader_class = nullptr;
    g_thread_class = nullptr;
    g_thread_current = nullptr;
    g_thread_get_context_loader = nullptr;
    g_thread_enumerate = nullptr;
    g_class_loader_load_class = nullptr;
    g_loader_bridge_failed = false;
    g_registry = nullptr;
    g_stats = CacheStats{};
}

jclass class_of(std::string_view class_alias) noexcept {
    if (g_registry == nullptr) {
        return nullptr;
    }

    const ClassEntry* found = g_registry->find_class(class_alias);
    if (found == nullptr) {
        report_missing_class(class_alias);
        return nullptr;
    }

    ClassEntry* entry = const_cast<ClassEntry*>(found);
    if (entry->java_class != nullptr) {
        return static_cast<jclass>(entry->java_class);
    }
    if (entry->class_failed) {
        return nullptr;
    }
    if (entry->intermediary_path.empty()) {
        entry->class_failed = true;
        ++g_stats.failures;
        WOKE_LOG_WARN(
            "jni: class mapping '%s' has no intermediary name", entry->simple_name.c_str());
        return nullptr;
    }

    JNIEnv* env = current_env();
    if (env == nullptr) {
        return nullptr;
    }
    return resolve_class(env, *entry);
}

jobject box_double(double value) noexcept {
    JNIEnv* env = current_env();
    if (env == nullptr || !ensure_double_bridge(env)) {
        return nullptr;
    }
    jobject boxed = env->CallStaticObjectMethod(
        g_double_class, g_double_value_of, static_cast<jdouble>(value));
    if (env->ExceptionCheck() != JNI_FALSE) {
        env->ExceptionClear();
        return nullptr;
    }
    return boxed;
}

bool unbox_double(jobject boxed, double& out) noexcept {
    JNIEnv* env = current_env();
    if (env == nullptr || boxed == nullptr || !ensure_double_bridge(env)) {
        return false;
    }
    // Called on the object, not on the class: a boxed Float or Integer therefore raises here and
    // is reported as a failed read instead of silently punned into a double.
    const jdouble value = env->CallDoubleMethod(boxed, g_double_value);
    if (env->ExceptionCheck() != JNI_FALSE) {
        env->ExceptionClear();
        return false;
    }
    out = static_cast<double>(value);
    return true;
}

jmethodID method_of(std::string_view class_alias, std::string_view member) noexcept {
    return resolve_method(class_alias, member, false);
}

jmethodID static_method_of(std::string_view class_alias, std::string_view member) noexcept {
    return resolve_method(class_alias, member, true);
}

jfieldID field_of(std::string_view class_alias, std::string_view member) noexcept {
    return resolve_field(class_alias, member);
}

CacheStats stats() noexcept {
    return g_stats;
}

} // namespace woke::jni
