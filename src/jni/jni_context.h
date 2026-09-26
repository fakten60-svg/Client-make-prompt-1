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
#include <cstdint>

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

// ── Local-reference bookkeeping (roadmap step 9, risk R-06) ──────────────────────────
//
// The soak gate is "no local-ref leaks (log line)". A ScopedLocalFrame that failed to push
// leaks every local its scope creates, and a pop that fails drops the frame it was supposed
// to release, so both are counted here and reported by the soak instrumentation. The counters
// are the *failures* - a frame that pushes and pops cleanly moves none of them - so a session
// total of zero is the pass condition, not a growing number to subtract.
struct LocalFrameStats {
    std::uint64_t pushed = 0;   // frames that pushed successfully
    std::uint64_t push_failed = 0;  // PushLocalFrame failed: scope leaks its locals
    std::uint64_t pop_failed = 0;   // PopLocalFrame failed: the frame was never released
};

[[nodiscard]] LocalFrameStats local_frame_stats() noexcept;

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
