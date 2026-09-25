#pragma once

// Notification pool (blueprint §7.5) - roadmap step 6.
//
// A fixed pool of eight toasts: slide in from the top-right, live for three seconds with a draining
// progress bar, slide out. Module enable/disable, config loads, mapping problems and perf warnings
// all surface through the one push() call site, which is what makes the UX consistent by
// construction rather than by everyone remembering the same layout numbers.
//
// Shape notes:
//
//   * Strings are copied into fixed buffers at push() time. The pool therefore holds no caller
//     memory, a temporary at the call site cannot dangle, and nothing here allocates (§6.3).
//   * Removal is animation-driven: a toast that has expired steers its reveal toward zero and is
//     only recycled once the state has actually settled, so it never disappears mid-slide.
//   * Portable (no windows.h): the host tests push toasts, step the clock and assert the stacking,
//     the lifetime and the recycling of a full pool.

#include <array>
#include <cstddef>
#include <cstdint>

#include "ui/animation/animation_controller.h"
#include "ui/components/base_component.h"
#include "utils/string_buffer.h"

namespace woke::ui {

// Which accent the toast carries. Three states is enough to distinguish "something happened" from
// "it worked" and "look at this", without an icon font the overlay would have to ship.
enum class ToastIcon : std::uint8_t {
    Info = 0,
    Success,
    Warning,
};

struct Toast {
    const char* title = nullptr;
    const char* message = nullptr;
    ToastIcon icon = ToastIcon::Info;
    float life_seconds = 3.0f;
};

class Notifications final : public components::BaseUIComponent {
public:
    static constexpr std::size_t kCapacity = 8;
    static constexpr std::size_t kTitleCapacity = 40;
    static constexpr std::size_t kMessageCapacity = 96;

    static constexpr float kWidth = 300.0f;
    static constexpr float kHeight = 60.0f;
    static constexpr float kMargin = 24.0f;
    static constexpr float kSpacing = 8.0f;
    static constexpr float kSlideDistance = 40.0f;
    static constexpr float kProgressHeight = 4.0f;
    static constexpr float kDefaultLifeSeconds = 3.0f;

    void bind(animation::AnimationController& controller) noexcept;
    void unbind() noexcept;
    void set_alpha(float alpha) noexcept;
    // The display rect the toasts hang from (top-right corner). Set once per frame by the GUI,
    // before input is dispatched, so hit-testing and drawing agree.
    void set_screen(const draw::Rect& screen) noexcept;

    // Returns false when the pool is full; dropped_count() then grows. A dropped toast is reported
    // rather than silently overwriting a live one, because overwriting is how a user misses the
    // message that mattered.
    bool push(const Toast& toast) noexcept;
    bool push(const char* title, const char* message,
        float life_seconds = kDefaultLifeSeconds) noexcept;

    void render(ImDrawList* draw_list, const draw::Rect& screen) noexcept override;
    bool handle_input(const components::Input& input) noexcept override;
    void animate(float delta_seconds) noexcept override;

    void clear() noexcept;

    // True while any toast is on screen or still animating out: the GUI folds this into its own
    // needs_render() so a toast keeps the overlay pipeline alive with the chrome hidden.
    [[nodiscard]] bool needs_render() const noexcept;
    [[nodiscard]] std::size_t active_count() const noexcept;
    [[nodiscard]] std::size_t dropped_count() const noexcept { return dropped_; }
    [[nodiscard]] bool active(std::size_t index) const noexcept;
    [[nodiscard]] float reveal_amount(std::size_t index) const noexcept;
    [[nodiscard]] float elapsed(std::size_t index) const noexcept;

    // Where the toast in slot `index` is drawn, given the screen rect. Public so input, drawing and
    // the host tests use one definition; the y position depends on how many older slots are still
    // on screen, which is what makes the stack collapse as toasts expire.
    [[nodiscard]] draw::Rect toast_rect(const draw::Rect& screen, std::size_t index) const noexcept;

private:
    struct Slot {
        util::FixedString<kTitleCapacity> title;
        util::FixedString<kMessageCapacity> message;
        float elapsed_seconds = 0.0f;
        float life_seconds = kDefaultLifeSeconds;
        ToastIcon icon = ToastIcon::Info;
        bool active = false;
        animation::StateHandle reveal{};
    };

    void release_slot(Slot& slot) noexcept;

    std::array<Slot, kCapacity> slots_{};
    draw::Rect screen_{};
    animation::AnimationController* controller_ = nullptr;
    float alpha_ = 1.0f;
    std::size_t dropped_ = 0;
};

} // namespace woke::ui
