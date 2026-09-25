#include "jni/game_writes_jni.h"

#include "jni/game_instance.h"
#include "modules/game_writes.h"

namespace woke::jni {
namespace {

// The baseline lives here rather than in the modules: a module that had to remember the user's
// original gamma would also have to survive a config reload that replaced it, and the whole point
// of the write seam is that a toggle is reversible without that bookkeeping.
class JniGameWrites final : public modules::GameWrites {
public:
    [[nodiscard]] game::Maybe<float> gamma() noexcept override { return game::gamma(); }
    [[nodiscard]] game::Maybe<float> fov() noexcept override { return game::fov(); }

    bool apply_gamma(float value) noexcept override {
        if (!gamma_saved_) {
            const game::Maybe<float> base = game::gamma();
            if (base.valid) {
                saved_gamma_ = base.value;
                gamma_saved_ = true;
            }
        }
        return game::set_gamma(value);
    }

    void restore_gamma() noexcept override {
        if (!gamma_saved_) {
            return;
        }
        (void)game::set_gamma(saved_gamma_);
        gamma_saved_ = false;
    }

    bool apply_fov(float degrees) noexcept override {
        if (!fov_saved_) {
            const game::Maybe<float> base = game::fov();
            if (base.valid) {
                saved_fov_ = base.value;
                fov_saved_ = true;
            }
        }
        return game::set_fov(degrees);
    }

    void restore_fov() noexcept override {
        if (!fov_saved_) {
            return;
        }
        (void)game::set_fov(saved_fov_);
        fov_saved_ = false;
    }

    [[nodiscard]] game::Maybe<bool> sprint_key_pressed() noexcept override {
        return game::sprint_key_pressed();
    }

    bool apply_sprint(bool pressed) noexcept override {
        if (!sprint_saved_) {
            const game::Maybe<bool> base = game::sprint_key_pressed();
            if (base.valid) {
                saved_sprint_ = base.value;
                sprint_saved_ = true;
            }
        }
        return game::set_sprint_key_pressed(pressed);
    }

    void restore_sprint() noexcept override {
        if (!sprint_saved_) {
            return;
        }
        (void)game::set_sprint_key_pressed(saved_sprint_);
        sprint_saved_ = false;
    }

private:
    bool gamma_saved_ = false;
    float saved_gamma_ = 0.0f;
    bool fov_saved_ = false;
    float saved_fov_ = 70.0f;
    bool sprint_saved_ = false;
    bool saved_sprint_ = false;
};

JniGameWrites g_jni_writes{};

} // namespace

modules::GameWrites& jni_game_writes() noexcept {
    return g_jni_writes;
}

} // namespace woke::jni
