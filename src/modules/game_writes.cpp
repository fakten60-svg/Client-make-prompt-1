#include "modules/game_writes.h"

// The recording backend and the process default. Portable on purpose: the host tests link this
// translation unit and the Windows DLL links it as the fallback the JNI installer replaces, so
// there is never more than one definition of these symbols in a target.

namespace woke::modules {
namespace {

RecordingGameWrites g_default_writes{};
GameWrites* g_active = &g_default_writes;

} // namespace

game::Maybe<float> RecordingGameWrites::gamma() noexcept {
    if (!available_) {
        return game::Maybe<float>::missing();
    }
    return game::Maybe<float>::of(gamma_value_);
}

game::Maybe<float> RecordingGameWrites::fov() noexcept {
    if (!available_) {
        return game::Maybe<float>::missing();
    }
    return game::Maybe<float>::of(fov_value_);
}

bool RecordingGameWrites::apply_gamma(float gamma_value) noexcept {
    if (!available_) {
        return false;
    }
    if (!gamma_captured_) {
        baseline_gamma_ = gamma_value_;
        gamma_captured_ = true;
    }
    gamma_active_ = true;
    gamma_value_ = gamma_value;
    ++gamma_applies_;
    return true;
}

void RecordingGameWrites::restore_gamma() noexcept {
    if (!gamma_active_) {
        return;
    }
    gamma_value_ = baseline_gamma_;
    gamma_active_ = false;
    ++restores_;
}

bool RecordingGameWrites::apply_fov(float degrees) noexcept {
    if (!available_) {
        return false;
    }
    if (!fov_captured_) {
        baseline_fov_ = fov_value_;
        fov_captured_ = true;
    }
    fov_active_ = true;
    fov_value_ = degrees;
    ++fov_applies_;
    return true;
}

void RecordingGameWrites::restore_fov() noexcept {
    if (!fov_active_) {
        return;
    }
    fov_value_ = baseline_fov_;
    fov_active_ = false;
    ++restores_;
}

void RecordingGameWrites::seed(float gamma_value, float fov_value) noexcept {
    gamma_value_ = gamma_value;
    fov_value_ = fov_value;
    gamma_captured_ = false;
    fov_captured_ = false;
    gamma_active_ = false;
    fov_active_ = false;
}

GameWrites* set_game_writes(GameWrites* writes) noexcept {
    GameWrites* previous = g_active;
    g_active = writes != nullptr ? writes : &g_default_writes;
    return previous;
}

GameWrites& game_writes() noexcept {
    return *g_active;
}

} // namespace woke::modules
