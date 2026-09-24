#pragma once

// Value mirrors for the Minecraft data the client reads.
//
// These are deliberately portable: no jni.h, no windows.h, no allocation. The JNI layer
// converts live Java objects into these PODs at its boundary, so a module never holds a
// jobject, never has to guard against a pending JNI exception, and never touches the
// heap. Everything a module sees is a plain value that is safe to copy, compare and
// store in a fixed buffer.

#include <cstdint>

namespace woke::game {

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct BlockPos {
    int x = 0;
    int y = 0;
    int z = 0;
};

// Mirrors net/minecraft/util/Hand (class_1268). "Invalid" exists so a failed lookup has
// an honest value instead of a silently wrong hand.
enum class Hand : std::int32_t { Main = 0, Off = 1, Invalid = -1 };

// "Optional by value".
//
// The JNI facade returns one of these instead of a nullable handle, which makes every
// module guard byte-for-byte identical:
//
//     const auto pos = woke::game::player_position();
//     if (!pos.valid) return;
//
// A missing player, an unloaded world and a stale mapping therefore all reduce to the
// same two lines - there is no null-dereference path and no exception path to forget.
template <typename T>
struct Maybe {
    T value{};
    bool valid = false;

    [[nodiscard]] static Maybe missing() noexcept { return Maybe{}; }

    [[nodiscard]] static Maybe of(const T& current) noexcept {
        Maybe result;
        result.value = current;
        result.valid = true;
        return result;
    }
};

} // namespace woke::game
