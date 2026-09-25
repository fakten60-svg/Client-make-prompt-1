#pragma once

// The JNI-backed client-write backend (roadmap step 7).
//
// Installed by the module boot step so Fullbright and Zoom write through the real game options;
// the portable recorder remains the fallback when the bridge is absent (WOKE_HAVE_JNI=0), which
// is what keeps the modules compiling and behaving identically in a process with no JVM.

#ifndef _WIN32
#error "game_writes_jni.h is Windows-only; portable translation units must not include it."
#endif

namespace woke::modules {
class GameWrites;
} // namespace woke::modules

namespace woke::jni {

// A process-lifetime backend, so the boot step can install it without owning storage. It captures
// each option's baseline on the first write and puts it back on restore.
[[nodiscard]] modules::GameWrites& jni_game_writes() noexcept;

} // namespace woke::jni
