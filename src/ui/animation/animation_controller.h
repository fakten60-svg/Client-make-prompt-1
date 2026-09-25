#pragma once

// Animation engine (blueprint §7.4).
//
// One controller owns every animated value in the client in two fixed arrays: exponentially
// approached "states" (toggles, hovers, chevrons, layout density) and critically damped springs
// (the ClickGUI open/close). Widgets acquire a handle once, at construction, and then a per-frame
// read is an index, a generation check and a float load - no map lookup, no string key, no
// allocation, which is the whole point of the design.
//
// Two properties are load-bearing:
//
//   * Delta-time driven. Motion is identical at 60 or 240 FPS; the host tests assert that
//     directly by stepping one 1/60 s frame against four 1/240 s frames.
//   * Generation-counted handles. A slot is recycled after release(), so a handle kept from a
//     previous enable could otherwise drive the widget that inherited its slot. Same failure
//     mode, same fix, as core/event_bus.h.
//
// Portable on purpose: no windows.h, no imgui.h, so the curves and the settling behaviour are
// asserted by the host test suite rather than only by watching the overlay.

#include <array>
#include <cstddef>
#include <cstdint>

namespace woke::ui::animation {

// Handle to one exponentially approached value. `index` is 1-based (0 means "invalid") so a
// default-constructed handle is recognisably empty, and the generation is what makes a stale
// handle a no-op instead of a misdirected write.
struct StateHandle {
    std::uint16_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0; }
    [[nodiscard]] friend constexpr bool operator==(const StateHandle&, const StateHandle&) noexcept
        = default;
};

struct SpringHandle {
    std::uint16_t index = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return index != 0; }
    [[nodiscard]] friend constexpr bool operator==(
        const SpringHandle&, const SpringHandle&) noexcept = default;
};

class AnimationController {
public:
    // The pool is sized for the widget library's worst case, not for a guess: a ModuleCard owns
    // five states (its own hover and drawer reveal, the pill's t, and the badge's hover and
    // capture pulse), so 32 cards alone are 160. The sidebar (12), the search field (2), the
    // toast pool (8), the traffic lights (6), the window's own collapse and density (2) and the
    // close spring's state bring the ceiling to ~190. 320 leaves real headroom, and exceeding it
    // is still reported by overflow_count() rather than silently dropping an animation - a
    // dropped state looks exactly like a widget stuck at its initial value.
    static constexpr std::size_t kMaxStates = 320;
    static constexpr std::size_t kMaxSprings = 24;

    static constexpr float kDefaultSpeed = 14.0f;      // ~90 ms to 90 % for a hover
    static constexpr float kDefaultStiffness = 170.0f; // critically damped at damping 2*sqrt(170)
    static constexpr float kDefaultDamping = 26.0f;

    // Springs are integrated in at most 1/120 s substeps: a 60 Hz frame is one step, a 20 FPS
    // frame is six, and the result is independent of how the frame times were distributed.
    static constexpr float kMaxSpringSubstep = 1.0f / 120.0f;

    void reset() noexcept;

    // Acquires a slot. Returns an invalid handle when the array is full (overflow_count grows).
    [[nodiscard]] StateHandle acquire_state(
        float value = 0.0f, float speed = kDefaultSpeed) noexcept;
    [[nodiscard]] SpringHandle acquire_spring(float value = 0.0f,
        float stiffness = kDefaultStiffness, float damping = kDefaultDamping) noexcept;

    // Releases a slot for reuse. The slot's generation advances, so a handle still in circulation
    // can no longer reach the new occupant.
    void release(StateHandle handle) noexcept;
    void release(SpringHandle handle) noexcept;

    [[nodiscard]] bool valid(StateHandle handle) const noexcept;
    [[nodiscard]] bool valid(SpringHandle handle) const noexcept;

    void set_target(StateHandle handle, float target) noexcept;
    void set_speed(StateHandle handle, float speed) noexcept;

    // Sets both the current value and the target: the "no animation, appear here" path used when
    // a GUI opens (nothing may slide in from a stale position).
    void snap(StateHandle handle, float value) noexcept;
    void snap(SpringHandle handle, float value) noexcept;

    // Jumps the current value without moving the target, so the next tick animates from here.
    void set_value(StateHandle handle, float value) noexcept;

    [[nodiscard]] float value(StateHandle handle) const noexcept;
    [[nodiscard]] float target(StateHandle handle) const noexcept;
    [[nodiscard]] bool settled(StateHandle handle, float epsilon = 0.002f) const noexcept;

    [[nodiscard]] float value(SpringHandle handle) const noexcept;
    [[nodiscard]] float target(SpringHandle handle) const noexcept;
    void set_target(SpringHandle handle, float target) noexcept;
    [[nodiscard]] bool settled(SpringHandle handle, float epsilon = 0.002f) const noexcept;
    [[nodiscard]] bool at_rest(SpringHandle handle, float epsilon = 0.002f) const noexcept;

    // Advances every live state and spring. A non-positive delta is ignored, so a paused frame
    // clock cannot rewind an animation.
    void tick(float delta_seconds) noexcept;

    [[nodiscard]] std::size_t active_state_count() const noexcept { return active_states_; }
    [[nodiscard]] std::size_t active_spring_count() const noexcept { return active_springs_; }
    [[nodiscard]] std::size_t overflow_count() const noexcept { return overflows_; }

private:
    struct State {
        float value = 0.0f;
        float target = 0.0f;
        float speed = kDefaultSpeed;
        std::uint32_t generation = 1;
        bool active = false;
    };

    struct Spring {
        float value = 0.0f;
        float velocity = 0.0f;
        float target = 0.0f;
        float stiffness = kDefaultStiffness;
        float damping = kDefaultDamping;
        std::uint32_t generation = 1;
        bool active = false;
    };

    [[nodiscard]] State* lookup(StateHandle handle) noexcept;
    [[nodiscard]] const State* lookup(StateHandle handle) const noexcept;
    [[nodiscard]] Spring* lookup(SpringHandle handle) noexcept;
    [[nodiscard]] const Spring* lookup(SpringHandle handle) const noexcept;

    std::array<State, kMaxStates> states_{};
    std::array<Spring, kMaxSprings> springs_{};
    std::size_t active_states_ = 0;
    std::size_t active_springs_ = 0;
    std::size_t overflows_ = 0;
};

} // namespace woke::ui::animation
