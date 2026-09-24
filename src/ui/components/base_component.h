#pragma once

// The component contract (blueprint §7.3).
//
// Every widget - traffic lights, pills, cards, nav rows, search, badges, toasts - implements this
// exact trio, which is what makes one component embeddable in the ClickGUI, the HUD and the toast
// pool without adaptation:
//
//   * render()       draws only. It never mutates state, so a frame can be re-rendered or skipped
//                    without side effects.
//   * handle_input() consumes only what it acts on and returns true when it did. Input is
//                    dispatched top-down, so a component that consumes a click stops it reaching
//                    the component underneath.
//   * animate()      advances the component's own AnimStates and never draws. Animation is frozen
//                    while the overlay is suppressed (§7.8) and resumes from current values.
//
// No windows.h: this header only needs ImGui's draw list and rectangle types, so the components
// are compiled and exercised by the host test suite on Linux.

#include <array>
#include <cstddef>

#include <imgui.h>

#include "utils/render_utils.h"

namespace woke::ui::components {

using draw::Rect;

// One frame's worth of pointer state, snapshotted by the GUI and passed to every component. Passing
// a snapshot rather than letting components query ImGui means a component's input handling is a
// pure function of this struct, which is what the host tests poke at.
struct Input {
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    std::array<bool, 3> down{false, false, false};
    std::array<bool, 3> pressed{false, false, false};
    std::array<bool, 3> released{false, false, false};
    float wheel = 0.0f;

    [[nodiscard]] bool is_down(std::size_t button) const noexcept {
        return button < down.size() && down[button];
    }
    [[nodiscard]] bool is_pressed(std::size_t button) const noexcept {
        return button < pressed.size() && pressed[button];
    }
    [[nodiscard]] bool is_released(std::size_t button) const noexcept {
        return button < released.size() && released[button];
    }
};

class BaseUIComponent {
public:
    BaseUIComponent() = default;
    virtual ~BaseUIComponent() = default;

    BaseUIComponent(const BaseUIComponent&) = delete;
    BaseUIComponent& operator=(const BaseUIComponent&) = delete;

    // noexcept throughout: the overlay draws inside the game's render thread, where an exception
    // escaping a widget would unwind through the game's own frame code.
    virtual void render(ImDrawList* draw_list, const Rect& area) noexcept = 0;
    virtual bool handle_input(const Input& input) noexcept = 0;
    virtual void animate(float delta_seconds) noexcept = 0;
};

} // namespace woke::ui::components
