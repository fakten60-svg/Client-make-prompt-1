#pragma once

// Value mirrors for the Minecraft data the client reads.
//
// These are deliberately portable: no jni.h, no windows.h, no allocation. The JNI layer
// converts live Java objects into these PODs at its boundary, so a module never holds a
// jobject, never has to guard against a pending JNI exception, and never touches the
// heap. Everything a module sees is a plain value that is safe to copy, compare and
// store in a fixed buffer.

#include <cstddef>
#include <cstdint>
#include <string_view>

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

// Fixed storage for the target's display name: bounded, no heap, copied out of JNI before
// the local reference dies. Capacity covers a nametag plus a null terminator; longer names
// truncate, which is what a HUD chip would do anyway.
struct FixedName {
    static constexpr std::size_t kCapacity = 64;

    char text[kCapacity] = {};
    std::size_t length = 0;

    void assign(std::string_view value) noexcept {
        length = value.size() < (kCapacity - 1) ? value.size() : (kCapacity - 1);
        for (std::size_t index = 0; index < length; ++index) {
            text[index] = value[index];
        }
        text[length] = '\0';
    }

    [[nodiscard]] const char* c_str() const noexcept { return text; }
    [[nodiscard]] bool empty() const noexcept { return length == 0; }
};

// What the client sees under its crosshair, flattened to values (roadmap step 8, Combat).
//
// The JNI layer turns a live crosshairTarget into this in one composite read, so a module
// never holds a jobject and the "entity died / left range / is not an entity" cases all
// reduce to the same missing() guard every other read uses. 0-length name means
// "an entity was targeted but its name read failed" - the HUD shows a plate without a
// name rather than inventing one.
struct TargetInfo {
    FixedName name{};
    float health = 0.0f;
    float max_health = 0.0f;
    float absorption = 0.0f;
    float distance = 0.0f;
    bool valid = false;
};

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
