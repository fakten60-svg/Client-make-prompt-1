#pragma once

// The single façade modules use to read live game state (blueprint §5.3).
//
// No module ever sees a JNIEnv, a jclass or a jobject. Every accessor returns a value type
// from game_types.h, so a module's code is a bounded read plus one `if (!value.valid)`
// guard, and a missing player / unloaded world / stale mapping cannot turn into a crash.
// This is also the only file (besides reflection_cache) that stores global refs, and it
// releases them in shutdown() in the reverse order it created them.

#ifndef _WIN32
#error "game_instance.h is Windows-only; portable translation units must not include it."
#endif

#include <cstddef>

#include "jni/game_types.h"

namespace woke::game {

// Resolves the handle table and caches the live MinecraftClient instance. Idempotent and
// retry-safe: it returns true immediately once the client is cached, and a false result
// (no JVM, or no client instance yet) may simply be retried later.
bool initialize() noexcept;

// Releases the cached global refs and clears the handle table.
void shutdown() noexcept;

[[nodiscard]] bool available() noexcept;

// True when both the client player and the client world are live, i.e. a world is loaded.
[[nodiscard]] bool in_world() noexcept;

// Members the mapping asset failed to provide. Non-zero means the corresponding accessors
// return `valid == false` forever and the modules that need them auto-disable.
[[nodiscard]] std::size_t unresolved_handle_count() noexcept;

[[nodiscard]] Maybe<Vec3d> player_position() noexcept;
[[nodiscard]] Maybe<Vec3d> player_velocity() noexcept;
[[nodiscard]] Maybe<Vec3d> player_eye_position() noexcept;
[[nodiscard]] Maybe<float> player_yaw() noexcept;
[[nodiscard]] Maybe<float> player_pitch() noexcept;
[[nodiscard]] Maybe<float> player_health() noexcept;
[[nodiscard]] Maybe<float> player_max_health() noexcept;
[[nodiscard]] Maybe<float> player_absorption() noexcept;
[[nodiscard]] Maybe<bool> player_on_ground() noexcept;
[[nodiscard]] Maybe<int> selected_hotbar_slot() noexcept;

[[nodiscard]] Maybe<int> world_entity_count() noexcept;
[[nodiscard]] Maybe<double> mouse_x() noexcept;
[[nodiscard]] Maybe<double> mouse_y() noexcept;

} // namespace woke::game
