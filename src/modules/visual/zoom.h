#pragma once

// Zoom (blueprint §8: Visual). Roadmap step 7.
//
// Narrows the field-of-view option toward `base / factor` while enabled, approaching the target
// from the current value so the change reads as a smooth zoom rather than a snap. Disabling
// restores the captured baseline through the write seam - the module never has to know it.
//
// A tick-rate-independent exponential approach would need the frame delta; on_tick runs at the
// game's fixed 20 Hz gate, so a plain per-tick blend is both deterministic and cheaper.

#include <array>

#include "modules/base_module.h"
#include "modules/game_writes.h"
#include "settings/setting.h"
#include "utils/math_utils.h"

namespace woke::modules::visual {

class Zoom final : public BaseModule {
public:
    static constexpr int kDefaultBind = 0x43; // 'C'

    Zoom() noexcept
        : BaseModule("Zoom", "Smooth field-of-view zoom while enabled.", Category::Visual,
            kDefaultBind) {
        register_settings(std::array<settings::Setting*, 2>{&factor_, &smoothness_});
    }

    bool on_enable() noexcept override {
        const game::Maybe<float> base = game_writes().fov();
        if (!base.valid || base.value <= 0.0f) {
            return false;
        }
        base_fov_ = base.value;
        current_ = base_fov_;
        written_ = -1.0f;
        apply_current();
        return true;
    }

    void on_disable() noexcept override {
        game_writes().restore_fov();
        written_ = -1.0f;
    }

    void on_tick(float /*delta_seconds*/) noexcept override {
        const float target = target_fov();
        const float blend = util::clamp01(smoothness_.value());
        current_ = blend >= 0.999f ? target : util::lerp(current_, target, blend);
        // Snap once the remaining error is below a tenth of a degree: an exponential approach
        // never quite arrives, and re-writing a value that only differs in the sixth decimal
        // would be a JNI call per tick for nothing.
        if (difference(current_, target) < 0.1f) {
            current_ = target;
        }
        apply_current();
    }

    [[nodiscard]] float factor() const noexcept { return factor_.value(); }

private:
    static float difference(float a, float b) noexcept { return a > b ? a - b : b - a; }

    [[nodiscard]] float target_fov() const noexcept {
        // A minimum of one degree keeps a large factor from crossing the near plane, which would
        // let the camera render inside the player's own head.
        const float target = base_fov_ / factor_.value();
        return target < 1.0f ? 1.0f : target;
    }

    void apply_current() noexcept {
        if (written_ >= 0.0f && difference(current_, written_) < 0.01f) {
            return;
        }
        if (game_writes().apply_fov(current_)) {
            written_ = current_;
        }
    }

    settings::SliderSetting factor_{"factor", "Zoom magnification", 3.0f, 1.5f, 8.0f, 0.5f};
    settings::SliderSetting smoothness_{
        "smoothness", "How quickly the zoom settles (higher is quicker)", 0.35f, 0.05f, 1.0f, 0.05f};

    float base_fov_ = 70.0f;
    float current_ = 70.0f;
    float written_ = -1.0f;
};

} // namespace woke::modules::visual
