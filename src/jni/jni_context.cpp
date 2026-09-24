#include "jni/jni_context.h"

#include <windows.h>

#include <psapi.h>

#include "core/logger.h"

namespace woke::jni {
namespace {

// jni.h is happy with any 1.x version; 1.8 is accepted by every JDK that can run
// Minecraft 1.21.11 and is the least surprising request.
constexpr jint kRequestedJniVersion = JNI_VERSION_1_8;

using GetCreatedJavaVMsFn = jint(JNICALL*)(JavaVM**, jsize, jsize*);

JavaVM* g_vm = nullptr;
bool g_lookup_done = false;
std::size_t g_attached_threads = 0;

thread_local JNIEnv* t_env = nullptr;
thread_local bool t_attached_by_us = false;

const wchar_t* base_name(const wchar_t* path) {
    const wchar_t* base = path;
    for (const wchar_t* cursor = path; *cursor != L'\0'; ++cursor) {
        if (*cursor == L'\\' || *cursor == L'/') {
            base = cursor + 1;
        }
    }
    return base;
}

bool is_jvm_module(const wchar_t* path) {
    return ::CompareStringOrdinal(base_name(path), -1, L"jvm.dll", -1, TRUE) == CSTR_EQUAL;
}

// Minecraft always runs in-process with jvm.dll loaded. We look it up by name first (fast
// path) and then scan the module list, because a launcher may map the JVM before naming it
// in the loader's module list order.
HMODULE find_jvm_module() {
    const HMODULE direct = ::GetModuleHandleW(L"jvm.dll");
    if (direct != nullptr) {
        return direct;
    }

    HMODULE modules[512];
    DWORD needed = 0;
    // sizeof is a size_t and EnumProcessModules takes a DWORD: the cast is explicit because
    // /W4 would otherwise make it a build error via /WX.
    if (::EnumProcessModules(::GetCurrentProcess(), modules,
            static_cast<DWORD>(sizeof(modules)), &needed) == FALSE) {
        return nullptr;
    }

    const std::size_t count = needed / sizeof(HMODULE);
    for (std::size_t index = 0; index < count; ++index) {
        wchar_t path[MAX_PATH] = {};
        if (::GetModuleFileNameW(modules[index], path, MAX_PATH) == 0) {
            continue;
        }
        if (is_jvm_module(path)) {
            return modules[index];
        }
    }
    return nullptr;
}

} // namespace

bool initialize() noexcept {
    if (g_lookup_done) {
        return g_vm != nullptr;
    }
    g_lookup_done = true;

    const HMODULE jvm_module = find_jvm_module();
    if (jvm_module == nullptr) {
        WOKE_LOG_WARN("jvm: no jvm.dll in this process - running console-only");
        return false;
    }

    // C4191: converting FARPROC to a specific signature is exactly what a dynamic
    // GetProcAddress lookup is, and the signature below is the one jvm.dll exports.
#pragma warning(push)
#pragma warning(disable : 4191)
    const auto get_created_vms = reinterpret_cast<GetCreatedJavaVMsFn>(
        ::GetProcAddress(jvm_module, "JNI_GetCreatedJavaVMs"));
#pragma warning(pop)

    if (get_created_vms == nullptr) {
        WOKE_LOG_ERROR("jvm: jvm.dll does not export JNI_GetCreatedJavaVMs");
        return false;
    }

    JavaVM* machines[4] = {};
    jsize count = 0;
    if (get_created_vms(machines, 4, &count) != JNI_OK || count == 0) {
        WOKE_LOG_WARN("jvm: the process has no live JVM yet");
        return false;
    }
    if (count > 1) {
        // Ambiguous: attaching to the wrong VM would mean reading another application's
        // memory, so this is refused rather than guessed.
        WOKE_LOG_ERROR("jvm: %d JVMs found - refusing to attach to an ambiguous host",
            static_cast<int>(count));
        return false;
    }

    g_vm = machines[0];
    if (current_env() == nullptr) {
        g_vm = nullptr;
        return false;
    }

    WOKE_LOG_INFO("jvm: attached to the host JVM (JNI %d.%d)",
        static_cast<int>(kRequestedJniVersion >> 16), static_cast<int>(kRequestedJniVersion & 0xFFFF));
    return true;
}

void shutdown() noexcept {
    detach_current_thread();
    g_vm = nullptr;
}

bool available() noexcept {
    return g_vm != nullptr;
}

JavaVM* vm() noexcept {
    return g_vm;
}

JNIEnv* current_env() noexcept {
    if (t_env != nullptr) {
        return t_env;
    }
    if (g_vm == nullptr) {
        return nullptr;
    }

    JNIEnv* env = nullptr;
    const jint state = g_vm->GetEnv(reinterpret_cast<void**>(&env), kRequestedJniVersion);
    if (state == JNI_OK && env != nullptr) {
        // The JVM attached this thread itself (the game thread): record the environment but
        // never detach a JVM-owned thread.
        t_env = env;
        return env;
    }

    if (state != JNI_EDETACHED) {
        WOKE_LOG_ERROR("jvm: GetEnv failed with state %d", static_cast<int>(state));
        return nullptr;
    }

    // Daemon attachment: a thread of ours must never be able to keep the host JVM alive
    // after the game closes.
    if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK
        || env == nullptr) {
        WOKE_LOG_ERROR("jvm: AttachCurrentThreadAsDaemon failed");
        return nullptr;
    }

    t_env = env;
    t_attached_by_us = true;
    ++g_attached_threads;
    return env;
}

void detach_current_thread() noexcept {
    if (t_env == nullptr) {
        return;
    }
    if (t_attached_by_us && g_vm != nullptr) {
        (void)g_vm->DetachCurrentThread();
        if (g_attached_threads > 0) {
            --g_attached_threads;
        }
    }
    t_env = nullptr;
    t_attached_by_us = false;
}

std::size_t attached_thread_count() noexcept {
    return g_attached_threads;
}

ScopedLocalFrame::ScopedLocalFrame(jint capacity) noexcept {
    env_ = current_env();
    if (env_ == nullptr) {
        return;
    }
    pushed_ = (env_->PushLocalFrame(capacity) == JNI_OK);
    if (!pushed_) {
        // A missing frame is a performance and leak concern, not a correctness one: JNI
        // still allocates the references, they are just not released as a block.
        WOKE_LOG_WARN("jvm: PushLocalFrame(%d) failed - continuing without a local frame",
            static_cast<int>(capacity));
    }
}

ScopedLocalFrame::~ScopedLocalFrame() noexcept {
    if (pushed_ && env_ != nullptr) {
        (void)env_->PopLocalFrame(nullptr);
    }
}

} // namespace woke::jni
