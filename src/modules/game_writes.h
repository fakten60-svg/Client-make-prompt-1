#pragma once

// Client-side game writes the visual modules perform (blueprint §8: Fullbright, Zoom).
//
// A portable seam, exactly like config's Storage interface: the modules call game_writes(), the
// JNI-backed implementation is installed by the boot sequence, and the process default is a
// recorder. That is what keeps a "toggling Fullbright asks for a gamma write and restores it on
// disable" assertion runnable in the host test suite, on a machine with no JVM at all.
//
// Only client-side, user-visible values flow through here: the gamma option, the field-of-view
// option and the sprint key's held state. Nothing in this interface can generate traffic or act
// without a toggle.
//
// Baseline contract (shared by the recorder and the JNI backend): the first apply_* captures the
// live value, and restore_* writes that captured value back. A toggle is therefore always
// reversible, even across a config reload, because the module never had to know the baseline.
//
// The sprint hold (roadmap step 8) is deliberately the same shape as a gamma write rather than a
// movement hack: Auto Sprint presses the game's *own* sprint key through the game's own key
// binding, so the vanilla input pipeline decides what sprinting means and no packet is ever
// synthesized. It is the least invasive thing that can make the player sprint.

#include <cstddef>

#include "jni/game_types.h"

namespace woke::modules {

class GameWrites {
public:
    virtual ~GameWrites() = default;

    // Reads the live option value. An invalid result means the mapping is stale or no world is
    // loaded yet, and the module refuses to enable rather than writing a guess.
    [[nodiscard]] virtual game::Maybe<float> gamma() noexcept = 0;
    [[nodiscard]] virtual game::Maybe<float> fov() noexcept = 0;

    // Returns false when the write could not be made; the caller keeps its own state honest
    // either way, so a missing handle is a silently absent feature rather than a lie.
    virtual bool apply_gamma(float gamma) noexcept = 0;
    virtual void restore_gamma() noexcept = 0;
    virtual bool apply_fov(float degrees) noexcept = 0;
    virtual void restore_fov() noexcept = 0;

    // The live "is the sprint key being held" state, read so the seam can capture it as the
    // baseline. An invalid result means the binding is unreachable, and Auto Sprint refuses to
    // enable rather than pretending to sprint.
    [[nodiscard]] virtual game::Maybe<bool> sprint_key_pressed() noexcept = 0;
    virtual bool apply_sprint(bool pressed) noexcept = 0;
    virtual void restore_sprint() noexcept = 0;

    // The same shape for the sneak key (roadmap step 8: Safe Walk). Sneaking is the game's own
    // "do not walk off this block" behaviour, so holding its key is the whole feature - no
    // movement override, and the vanilla edge protection does the work.
    [[nodiscard]] virtual game::Maybe<bool> sneak_key_pressed() noexcept = 0;
    virtual bool apply_sneak(bool pressed) noexcept = 0;
    virtual void restore_sneak() noexcept = 0;
};

// Installs a backend and returns the previous one (config::set_storage's shape, deliberately).
// Passing nullptr restores the built-in recorder.
[[nodiscard]] GameWrites* set_game_writes(GameWrites* writes) noexcept;

// Never null: the process default records the last request until a backend is installed.
[[nodiscard]] GameWrites& game_writes() noexcept;

// Recording backend: the process default, and what the host tests install to observe a module's
// intent without a JVM.
class RecordingGameWrites final : public GameWrites {
public:
    [[nodiscard]] game::Maybe<float> gamma() noexcept override;
    [[nodiscard]] game::Maybe<float> fov() noexcept override;
    bool apply_gamma(float gamma) noexcept override;
    void restore_gamma() noexcept override;
    bool apply_fov(float degrees) noexcept override;
    void restore_fov() noexcept override;
    [[nodiscard]] game::Maybe<bool> sprint_key_pressed() noexcept override;
    bool apply_sprint(bool pressed) noexcept override;
    void restore_sprint() noexcept override;
    [[nodiscard]] game::Maybe<bool> sneak_key_pressed() noexcept override;
    bool apply_sneak(bool pressed) noexcept override;
    void restore_sneak() noexcept override;

    // Test controls. `seed` is what the live option currently holds: the next apply_* captures it
    // as the baseline, and set_available(false) models "the mapping did not resolve".
    void seed(float gamma_value, float fov_value) noexcept;
    void seed_sprint(bool pressed) noexcept;
    void seed_sneak(bool pressed) noexcept;
    void set_available(bool available) noexcept { available_ = available; }

    [[nodiscard]] bool available() const noexcept { return available_; }
    [[nodiscard]] bool gamma_active() const noexcept { return gamma_active_; }
    [[nodiscard]] bool fov_active() const noexcept { return fov_active_; }
    [[nodiscard]] float gamma_value() const noexcept { return gamma_value_; }
    [[nodiscard]] float fov_value() const noexcept { return fov_value_; }
    [[nodiscard]] bool sprint_active() const noexcept { return sprint_active_; }
    [[nodiscard]] bool sprint_pressed() const noexcept { return sprint_pressed_; }
    [[nodiscard]] bool baseline_sprint() const noexcept { return baseline_sprint_; }
    [[nodiscard]] std::size_t sprint_applies() const noexcept { return sprint_applies_; }
    [[nodiscard]] bool sneak_active() const noexcept { return sneak_active_; }
    [[nodiscard]] bool sneak_pressed() const noexcept { return sneak_pressed_; }
    [[nodiscard]] bool baseline_sneak() const noexcept { return baseline_sneak_; }
    [[nodiscard]] std::size_t sneak_applies() const noexcept { return sneak_applies_; }
    [[nodiscard]] float baseline_gamma() const noexcept { return baseline_gamma_; }
    [[nodiscard]] float baseline_fov() const noexcept { return baseline_fov_; }
    [[nodiscard]] std::size_t gamma_applies() const noexcept { return gamma_applies_; }
    [[nodiscard]] std::size_t fov_applies() const noexcept { return fov_applies_; }
    [[nodiscard]] std::size_t restores() const noexcept { return restores_; }

private:
    bool available_ = true;
    float baseline_gamma_ = 0.0f;
    float baseline_fov_ = 70.0f;
    float gamma_value_ = 0.0f;
    float fov_value_ = 70.0f;
    bool gamma_captured_ = false;
    bool fov_captured_ = false;
    bool gamma_active_ = false;
    bool fov_active_ = false;
    std::size_t gamma_applies_ = 0;
    std::size_t fov_applies_ = 0;
    std::size_t restores_ = 0;

    bool baseline_sprint_ = false;
    bool sprint_pressed_ = false;
    bool sprint_captured_ = false;
    bool sprint_active_ = false;
    std::size_t sprint_applies_ = 0;

    bool baseline_sneak_ = false;
    bool sneak_pressed_ = false;
    bool sneak_captured_ = false;
    bool sneak_active_ = false;
    std::size_t sneak_applies_ = 0;
};

} // namespace woke::modules
