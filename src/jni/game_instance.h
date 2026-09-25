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

// True when the client's player reference is set. Separate from in_world() so the frame
// scheduler can publish a player transition that is genuinely its own signal.
[[nodiscard]] bool has_player() noexcept;

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

// ── Client-side option reads and writes (roadmap step 7) ─────────────────────────
//
// The gamma and field-of-view options, read and written through the game's own SimpleOption
// setValue path so the game's own clamping and change callbacks still run. These are the only
// writes the client performs, and they are exactly what Fullbright and Zoom are built on: a
// value goes in while the module is enabled and the seam captures the original to put back.
[[nodiscard]] Maybe<float> gamma() noexcept;
[[nodiscard]] Maybe<float> fov() noexcept;
[[nodiscard]] bool set_gamma(float gamma) noexcept;
[[nodiscard]] bool set_fov(float degrees) noexcept;

// ── The sprint key (roadmap step 8) ──────────────────────────────────────────────
//
// The client's sprint key binding, read and written through the game's own KeyBinding, so vanilla
// input handling (canSprint/shouldStopSprinting and the sprint packet the game itself sends) is
// what responds. Auto Sprint is therefore a *key hold*, not a movement override: the client never
// synthesizes a packet, and releasing restores whatever the user's own key was doing.
[[nodiscard]] Maybe<bool> sprint_key_pressed() noexcept;
[[nodiscard]] bool set_sprint_key_pressed(bool pressed) noexcept;

} // namespace woke::game
