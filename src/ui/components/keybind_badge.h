#pragma once

// Keybind badge (blueprint §3.6, §7.1) - roadmap step 6.
//
// The `[KEY: X]` chip every module card carries, plus the capture state machine that makes binds
// rebindable: clicking the chip arms it, the next key the window procedure routes becomes the new
// bind, and ESC cancels. Exactly one capture flag lives here, so two cards can never both be
// waiting for a key - the GUI arms one badge and asks it first.
//
// Portable (no windows.h): the key-name table is a plain switch over virtual-key codes, which is
// precisely the part worth unit-testing on the host. A wrong label is a user-visible bug, and a
// wrong code mapping would silently bind a different key than the chip advertises.

#include <cstddef>

#include "ui/animation/animation_controller.h"
#include "ui/components/base_component.h"

namespace woke::ui::components {

// The virtual-key codes the badge names. Spelled out rather than pulled from <windows.h> so this
// component compiles on the Linux test runner; the values are fixed Win32 ABI and cannot drift.
enum : int {
    kKeyBackspace = 0x08,
    kKeyTab = 0x09,
    kKeyEnter = 0x0D,
    kKeyShift = 0x10,
    kKeyControl = 0x11,
    kKeyAltMenu = 0x12,
    kKeyCapsLock = 0x14,
    kKeyEscape = 0x1B,
    kKeySpace = 0x20,
    kKeyPageUp = 0x21,
    kKeyPageDown = 0x22,
    kKeyEnd = 0x23,
    kKeyHome = 0x24,
    kKeyLeft = 0x25,
    kKeyUp = 0x26,
    kKeyRight = 0x27,
    kKeyDown = 0x28,
    kKeyInsert = 0x2D,
    kKeyDelete = 0x2E,
    kKeyLeftShift = 0xA0,
    kKeyRightShift = 0xA1,
    kKeyLeftControl = 0xA2,
    kKeyRightControl = 0xA3,
    kKeyLeftAlt = 0xA4,
    kKeyRightAlt = 0xA5,
    kKeyF1 = 0x70,
    kKeyF12 = 0x7B,
};

// Human label for a virtual key, written into `out` (never overruns, always terminated):
// "RSHIFT", "SPACE", "F5", "A", "0". Code 0 is "NONE"; anything unnamed falls back to "0xNN",
// which is still more useful to the user than an empty chip.
void format_key(int virtual_key, char* out, std::size_t capacity) noexcept;

class KeybindBadge final : public BaseUIComponent {
public:
    void bind(animation::AnimationController& controller) noexcept;
    void unbind() noexcept;

    void set_area(const Rect& area) noexcept;
    void set_bind(int virtual_key) noexcept;
    // Arming is the GUI's decision: it owns "which card is capturing" and tells this badge.
    void set_capture_armed(bool armed) noexcept;
    void set_alpha(float alpha) noexcept;

    [[nodiscard]] int bind_key() const noexcept { return bind_; }
    [[nodiscard]] bool capture_armed() const noexcept { return armed_; }
    [[nodiscard]] bool empty() const noexcept { return bind_ == 0; }

    // Turns one routed key into a rebind or a cancel. Returns true when the badge consumed the key,
    // in which case the caller must swallow it - it is the user's new bind, not a game input.
    //   ESC            -> cancel, the bind is unchanged, consumed
    //   any other code -> the bind becomes that code, consumed
    // Each outcome disarms. An unarmed badge consumes nothing.
    bool accept_key(int virtual_key) noexcept;

    void render(ImDrawList* draw_list, const Rect& area) noexcept override;
    bool handle_input(const Input& input) noexcept override;
    void animate(float delta_seconds) noexcept override;

    // One-shot: the chip was clicked in this input frame. The GUI arms capture in response, so the
    // click cannot also toggle the card underneath.
    [[nodiscard]] bool take_click() noexcept;

    [[nodiscard]] float hover_amount() const noexcept;
    [[nodiscard]] float pulse_amount() const noexcept;

private:
    animation::AnimationController* controller_ = nullptr;
    animation::StateHandle hover_{};
    animation::StateHandle pulse_{};
    Rect area_{};
    int bind_ = 0;
    bool armed_ = false;
    bool hovered_ = false;
    bool clicked_ = false;
    float alpha_ = 1.0f;
};

} // namespace woke::ui::components
