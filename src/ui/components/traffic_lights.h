#pragma once

// macOS window controls (blueprint §3.6, §7.1).
//
// Three circles - close #FF5F56, minimize #FFBD2E, zoom #27C93F - each with its own eased hover
// brightness and press state. One AnimState pair per button rather than one shared hover value,
// because a shared value is exactly how two buttons end up lit at once.
//
// Close hides the GUI (it never unloads the client), minimize collapses the window to a pill that
// stays clickable, and zoom toggles the content layout between grid and list density - the same
// mechanism the module grid uses in step 6.

#include <array>
#include <cstddef>

#include "ui/animation/animation_controller.h"
#include "ui/components/base_component.h"

namespace woke::ui::components {

class TrafficLights final : public BaseUIComponent {
public:
    static constexpr std::size_t kCount = 3;

    enum class Action {
        None,
        Close,
        Minimize,
        Zoom,
    };

    TrafficLights() = default;

    // Acquires this component's animation slots. Called once, when the GUI is initialised; the
    // controller is not copyable state, so the component holds a pointer rather than a reference
    // to keep TrafficLights trivially assignable inside the GUI's state struct.
    void bind(animation::AnimationController& controller) noexcept;
    void unbind() noexcept;

    // Layout step, run by the GUI before input is dispatched: hit-testing has to be a pure
    // function of the frame's geometry, which is what lets handle_input() stay area-free.
    void set_area(const Rect& area) noexcept;

    void render(ImDrawList* draw_list, const Rect& area) noexcept override;
    bool handle_input(const Input& input) noexcept override;

    // Steers this component's targets. The shared AnimationController is ticked exactly once per
    // frame by the GUI, so a component can never advance the clock twice.
    void animate(float delta_seconds) noexcept override;

    // One-shot: returns the action produced by the last input frame and clears it, so a click can
    // never be acted on twice (once by the click frame, once by the release frame).
    [[nodiscard]] Action take_action() noexcept;

    [[nodiscard]] float hover_amount(std::size_t index) const noexcept;
    [[nodiscard]] float press_amount(std::size_t index) const noexcept;

    // Where button `index` sits inside `area`. Public because the GUI draws its own header hit
    // test around it (a drag must start in the header but not on a traffic light).
    [[nodiscard]] Rect button_rect(const Rect& area, std::size_t index) const noexcept;

private:
    [[nodiscard]] static Action action_for(std::size_t index) noexcept;
    [[nodiscard]] int hit_test(const Rect& area, float x, float y) const noexcept;

    animation::AnimationController* controller_ = nullptr;
    std::array<animation::StateHandle, kCount> hover_{};
    std::array<animation::StateHandle, kCount> press_{};
    std::array<bool, kCount> hovered_{false, false, false};
    Rect area_{};
    Action pending_ = Action::None;
    int armed_ = -1; // the button the current press started on
};

} // namespace woke::ui::components
