#pragma once

// JVM discovery and per-thread attach discipline (blueprint §5.1).
//
// Windows + JNI only: this header includes jni.h, so portable translation units must
// never include it. The JVM is found with JNI_GetCreatedJavaVMs resolved from the jvm.dll
// that Minecraft already loaded - we never link against an import library, which keeps the
// DLL loadable in any process (it simply reports "no JVM" and stays console-only).

#ifndef _WIN32
#error "jni_context.h is Windows-only; portable translation units must not include it."
#endif

#include <jni.h>
#include <cstddef>

namespace woke::jni {

// Locates jvm.dll, resolves JNI_GetCreatedJavaVMs and attaches the calling thread.
// Returns false (with a logged reason) when the host process has no JVM, which is the
// expected result for a happy-path load into a non-Java process.
bool initialize() noexcept;

// Releases the current thread's attachment. Global refs are released by their owners
// (reflection_cache, game_instance) before this is called.
void shutdown() noexcept;

[[nodiscard]] bool available() noexcept;
[[nodiscard]] JavaVM* vm() noexcept;

// JNIEnv for the calling thread, attaching it as a daemon on first use.
//
// The attachment is deliberately thread-local: a JNIEnv* is only valid on the thread that
// obtained it. Today that is the worker thread; from roadmap step 3 the game thread calls
// these same accessors from inside the swap hook, and each thread transparently gets its
// own environment.
[[nodiscard]] JNIEnv* current_env() noexcept;

// Detaches the calling thread if *we* attached it. A thread that was already attached by
// the JVM itself (the game thread, for instance) is left alone.
void detach_current_thread() noexcept;

[[nodiscard]] std::size_t attached_thread_count() noexcept;

// RAII local-reference frame.
//
// Per-frame JNI locals are released as a block. This is the reason no other file in the
// project contains a DeleteLocalRef: with one frame per read there is no per-reference
// bookkeeping to get wrong, and a long session cannot leak local refs into the JVM.
class ScopedLocalFrame {
public:
    static constexpr jint kDefaultCapacity = 24;

    explicit ScopedLocalFrame(jint capacity = kDefaultCapacity) noexcept;
    ~ScopedLocalFrame() noexcept;

    ScopedLocalFrame(const ScopedLocalFrame&) = delete;
    ScopedLocalFrame& operator=(const ScopedLocalFrame&) = delete;

    [[nodiscard]] bool valid() const noexcept { return pushed_; }

private:
    JNIEnv* env_ = nullptr;
    bool pushed_ = false;
};

} // namespace woke::jni
