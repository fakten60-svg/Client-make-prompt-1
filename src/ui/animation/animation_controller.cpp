#include "ui/animation/animation_controller.h"

#include <cmath>

#include "utils/math_utils.h"

namespace woke::ui::animation {
namespace {

// Exponential approach: v += (target - v) * (1 - e^(-speed * dt)).
//
// Chosen over a fixed per-frame step because it is exactly frame-rate independent - stepping
// 1/60 s once lands on the same value as stepping 1/240 s four times, to float precision - which
// is what the host test asserts and what stops a 240 Hz client from animating noticeably faster
// than a 60 Hz one.
void advance_state(float& value, float target, float speed, float delta_seconds) noexcept {
    if (!(delta_seconds > 0.0f) || !(speed > 0.0f)) {
        return;
    }
    const float decay = std::exp(-speed * delta_seconds);
    value = target + ((value - target) * decay);
    if (std::fabs(target - value) < 0.00005f) {
        value = target; // settle exactly, so `settled()` is reachable
    }
}

// Semi-implicit critically damped spring, integrated in bounded substeps.
//
// Substepping is not decoration: a 0.25 s frame delta (the scheduler's clamp) with a single
// explicit step makes a stiff spring explode, and an overlay that launches itself off screen
// because the game hitched is a worse bug than a slightly slower open animation.
void advance_spring(float& value, float& velocity, float target, float stiffness, float damping,
    float delta_seconds) noexcept {
    if (!(delta_seconds > 0.0f)) {
        return;
    }

    float remaining = delta_seconds;
    while (remaining > 0.0f) {
        const float step = remaining > AnimationController::kMaxSpringSubstep
            ? AnimationController::kMaxSpringSubstep
            : remaining;
        remaining -= step;

        const float force = ((target - value) * stiffness) - (velocity * damping);
        velocity += force * step;
        value += velocity * step;
    }

    if ((std::fabs(target - value) < 0.0002f) && (std::fabs(velocity) < 0.01f)) {
        value = target;
        velocity = 0.0f; // at rest, so the close path can stop rendering
    }
}

} // namespace

void AnimationController::reset() noexcept {
    states_ = {};
    springs_ = {};
    for (State& state : states_) {
        state.generation = 1;
    }
    for (Spring& spring : springs_) {
        spring.generation = 1;
    }
    active_states_ = 0;
    active_springs_ = 0;
    overflows_ = 0;
}

StateHandle AnimationController::acquire_state(float value, float speed) noexcept {
    for (std::size_t offset = 0; offset < kMaxStates; ++offset) {
        State& state = states_[offset];
        if (state.active) {
            continue;
        }
        state.active = true;
        state.value = value;
        state.target = value;
        state.speed = speed > 0.0f ? speed : kDefaultSpeed;
        ++active_states_;
        return StateHandle{static_cast<std::uint16_t>(offset + 1), state.generation};
    }

    ++overflows_;
    return StateHandle{};
}

SpringHandle AnimationController::acquire_spring(
    float value, float stiffness, float damping) noexcept {
    for (std::size_t offset = 0; offset < kMaxSprings; ++offset) {
        Spring& spring = springs_[offset];
        if (spring.active) {
            continue;
        }
        spring.active = true;
        spring.value = value;
        spring.velocity = 0.0f;
        spring.target = value;
        spring.stiffness = stiffness > 0.0f ? stiffness : kDefaultStiffness;
        spring.damping = damping > 0.0f ? damping : kDefaultDamping;
        ++active_springs_;
        return SpringHandle{static_cast<std::uint16_t>(offset + 1), spring.generation};
    }

    ++overflows_;
    return SpringHandle{};
}

void AnimationController::release(StateHandle handle) noexcept {
    State* state = lookup(handle);
    if (state == nullptr) {
        return;
    }
    // Bumps the generation so the handle that was just released cannot reach the next occupant.
    ++state->generation;
    state->active = false;
    state->value = 0.0f;
    state->target = 0.0f;
    --active_states_;
}

void AnimationController::release(SpringHandle handle) noexcept {
    Spring* spring = lookup(handle);
    if (spring == nullptr) {
        return;
    }
    ++spring->generation;
    spring->active = false;
    spring->value = 0.0f;
    spring->velocity = 0.0f;
    spring->target = 0.0f;
    --active_springs_;
}

bool AnimationController::valid(StateHandle handle) const noexcept {
    return lookup(handle) != nullptr;
}

bool AnimationController::valid(SpringHandle handle) const noexcept {
    return lookup(handle) != nullptr;
}

void AnimationController::set_target(StateHandle handle, float target) noexcept {
    if (State* state = lookup(handle); state != nullptr) {
        state->target = target;
    }
}

void AnimationController::set_speed(StateHandle handle, float speed) noexcept {
    if (State* state = lookup(handle); state != nullptr) {
        state->speed = speed > 0.0f ? speed : kDefaultSpeed;
    }
}

void AnimationController::snap(StateHandle handle, float value) noexcept {
    if (State* state = lookup(handle); state != nullptr) {
        state->value = value;
        state->target = value;
    }
}

void AnimationController::snap(SpringHandle handle, float value) noexcept {
    if (Spring* spring = lookup(handle); spring != nullptr) {
        spring->value = value;
        spring->target = value;
        spring->velocity = 0.0f;
    }
}

void AnimationController::set_value(StateHandle handle, float value) noexcept {
    if (State* state = lookup(handle); state != nullptr) {
        state->value = value;
    }
}

float AnimationController::value(StateHandle handle) const noexcept {
    const State* state = lookup(handle);
    return state != nullptr ? state->value : 0.0f;
}

float AnimationController::target(StateHandle handle) const noexcept {
    const State* state = lookup(handle);
    return state != nullptr ? state->target : 0.0f;
}

bool AnimationController::settled(StateHandle handle, float epsilon) const noexcept {
    const State* state = lookup(handle);
    if (state == nullptr) {
        return true;
    }
    return std::fabs(state->target - state->value) <= epsilon;
}

float AnimationController::value(SpringHandle handle) const noexcept {
    const Spring* spring = lookup(handle);
    return spring != nullptr ? spring->value : 0.0f;
}

float AnimationController::target(SpringHandle handle) const noexcept {
    const Spring* spring = lookup(handle);
    return spring != nullptr ? spring->target : 0.0f;
}

void AnimationController::set_target(SpringHandle handle, float target) noexcept {
    if (Spring* spring = lookup(handle); spring != nullptr) {
        spring->target = target;
    }
}

bool AnimationController::settled(SpringHandle handle, float epsilon) const noexcept {
    const Spring* spring = lookup(handle);
    if (spring == nullptr) {
        return true;
    }
    return std::fabs(spring->target - spring->value) <= epsilon;
}

bool AnimationController::at_rest(SpringHandle handle, float epsilon) const noexcept {
    const Spring* spring = lookup(handle);
    if (spring == nullptr) {
        return true;
    }
    return std::fabs(spring->target - spring->value) <= epsilon
        && std::fabs(spring->velocity) <= 0.01f;
}

void AnimationController::tick(float delta_seconds) noexcept {
    if (!(delta_seconds > 0.0f)) {
        return;
    }

    for (State& state : states_) {
        if (state.active) {
            advance_state(state.value, state.target, state.speed, delta_seconds);
        }
    }

    for (Spring& spring : springs_) {
        if (spring.active) {
            advance_spring(spring.value, spring.velocity, spring.target, spring.stiffness,
                spring.damping, delta_seconds);
        }
    }
}

AnimationController::State* AnimationController::lookup(StateHandle handle) noexcept {
    if (!handle.valid() || handle.index > kMaxStates) {
        return nullptr;
    }
    State& state = states_[handle.index - 1];
    return (state.active && state.generation == handle.generation) ? &state : nullptr;
}

const AnimationController::State* AnimationController::lookup(StateHandle handle) const noexcept {
    if (!handle.valid() || handle.index > kMaxStates) {
        return nullptr;
    }
    const State& state = states_[handle.index - 1];
    return (state.active && state.generation == handle.generation) ? &state : nullptr;
}

AnimationController::Spring* AnimationController::lookup(SpringHandle handle) noexcept {
    if (!handle.valid() || handle.index > kMaxSprings) {
        return nullptr;
    }
    Spring& spring = springs_[handle.index - 1];
    return (spring.active && spring.generation == handle.generation) ? &spring : nullptr;
}

const AnimationController::Spring* AnimationController::lookup(SpringHandle handle) const noexcept {
    if (!handle.valid() || handle.index > kMaxSprings) {
        return nullptr;
    }
    const Spring& spring = springs_[handle.index - 1];
    return (spring.active && spring.generation == handle.generation) ? &spring : nullptr;
}

} // namespace woke::ui::animation
