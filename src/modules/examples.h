#pragma once

// Example modules (roadmap step 5 gate: "two dummy modules toggle, persist across reinjection").
//
// These are the reference implementations for every later module file: BaseModule subclass,
// settings declared as members, register_settings() last in the constructor. Game interaction
// arrives in steps 7-8; a constructor never touches the JVM (§4.4), so these two compile and
// behave identically with or without the bridge - which is exactly what makes them useful as
// persistence and UI fixtures.

#include <array>

#include "modules/base_module.h"
#include "settings/setting.h"

namespace woke::modules {

class SprintState final : public BaseModule {
public:
    SprintState() noexcept
        : BaseModule("Sprint State", "Mirrors the client-side sprint flag (example module).",
            Category::Movement, 0) {
        register_settings(std::array<settings::Setting*, 1>{&log_transitions_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

private:
    settings::BoolSetting log_transitions_{
        "log_transitions", "Log sprint state transitions (example bool setting)", false};
};

class ZoomAmount final : public BaseModule {
public:
    static constexpr int kDefaultBind = 0x43; // 'C'

    ZoomAmount() noexcept
        : BaseModule("Zoom Amount", "Smooth camera zoom factor (example slider + enum).",
            Category::Visual, kDefaultBind) {
        register_settings(std::array<settings::Setting*, 2>{&factor_, &curve_});
    }

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] float factor() const noexcept { return factor_.value(); }

private:
    settings::SliderSetting factor_{"factor", "Zoom magnification", 3.0f, 1.5f, 8.0f, 0.1f};

    static constexpr const char* kCurves[] = {"linear", "smooth", "instant"};
    settings::EnumSetting curve_{"curve", "Zoom interpolation curve", kCurves, 1};
};

} // namespace woke::modules
