#include "test_harness.h"

// The portable half of roadmap step 4: the curves and the animation engine the overlay is built on,
// plus the theme tokens the whole UI is defined by. All of this runs on the Linux runner in
// milliseconds, which is the point - a curve that never settles or a recycled animation slot is
// exactly the kind of bug that is invisible in a screenshot and obvious in a test.

#include "ui/animation/animation_controller.h"
#include "ui/animation/easing.h"
#include "ui/theme.h"
#include "utils/math_utils.h"

namespace {

using woke::ui::animation::AnimationController;
using woke::ui::animation::SpringHandle;
using woke::ui::animation::StateHandle;
using woke::ui::animation::ease_in_out_cubic;
using woke::ui::animation::ease_out_back;
using woke::ui::animation::ease_out_cubic;
using woke::ui::animation::ease_out_expo;
using woke::ui::animation::ease_out_quint;
using woke::util::Rgba;

// How many modules the blueprint's catalogue (§8) assigns across the six categories.
constexpr std::size_t kCatalogTotal = 24;

int count_modules_in_catalog() {
    int total = 0;
    for (std::size_t index = 0; index < woke::ui::kModuleCategoryCount; ++index) {
        total += static_cast<int>(
            woke::ui::category_catalog_size(static_cast<woke::ui::Section>(index)));
    }
    return total;
}

// One 1/60 s frame against four 1/240 s frames: the animation must land on the same value.
float advance_once(float seconds) {
    AnimationController controller;
    const StateHandle handle = controller.acquire_state(0.0f, 12.0f);
    controller.set_target(handle, 1.0f);
    controller.tick(seconds);
    return controller.value(handle);
}

float advance_many(float seconds, int steps) {
    AnimationController controller;
    const StateHandle handle = controller.acquire_state(0.0f, 12.0f);
    controller.set_target(handle, 1.0f);
    for (int step = 0; step < steps; ++step) {
        controller.tick(seconds / static_cast<float>(steps));
    }
    return controller.value(handle);
}

} // namespace

void test_ui_animation() {
    // ── math_utils ────────────────────────────────────────────────────────────────
    woke_test::section("ui math_utils");
    WOKE_CHECK(woke::util::clamp(5.0f, 0.0f, 1.0f) == 1.0f);
    WOKE_CHECK(woke::util::clamp(-5.0f, 0.0f, 1.0f) == 0.0f);
    WOKE_CHECK(woke::util::clamp01(0.25f) == 0.25f);
    WOKE_CHECK(woke::util::lerp(10.0f, 20.0f, 0.5f) == 15.0f);
    WOKE_CHECK(woke::util::inverse_lerp(10.0f, 20.0f, 15.0f) == 0.5f);
    WOKE_CHECK(woke::util::inverse_lerp(1.0f, 1.0f, 5.0f) == 0.0f); // degenerate range
    WOKE_CHECK(woke::util::approach(0.0f, 10.0f, 3.0f) == 3.0f);
    WOKE_CHECK(woke::util::approach(9.0f, 10.0f, 3.0f) == 10.0f);
    WOKE_CHECK(woke::util::nearly_equal(1.0f, 1.0001f));

    // The palette is written as hex, so a token and its channel values must agree exactly.
    const Rgba apple_blue = woke::util::from_hex(0x0A84FF);
    WOKE_CHECK(woke::util::nearly_equal(apple_blue.r, 10.0f / 255.0f));
    WOKE_CHECK(woke::util::nearly_equal(apple_blue.g, 132.0f / 255.0f));
    WOKE_CHECK(woke::util::nearly_equal(apple_blue.b, 255.0f / 255.0f));
    WOKE_CHECK(apple_blue.a == 1.0f);
    WOKE_CHECK(woke::util::from_hex(0x0B0E14, 0.90f).a == 0.90f);

    const Rgba blended = woke::util::mix(woke::util::kBlack, woke::util::kWhite, 0.5f);
    WOKE_CHECK(woke::util::nearly_equal(blended.r, 0.5f));
    WOKE_CHECK(woke::util::nearly_equal(blended.g, 0.5f));
    WOKE_CHECK(woke::util::nearly_equal(blended.b, 0.5f));
    WOKE_CHECK(woke::util::mix(woke::util::kBlack, woke::util::kWhite, 5.0f).r == 1.0f); // clamped
    WOKE_CHECK(woke::util::with_alpha(woke::util::kWhite, 4.0f).a == 1.0f);
    WOKE_CHECK(woke::util::scale_alpha(woke::util::from_hex(0xFFFFFF, 0.5f), 0.5f).a == 0.25f);
    WOKE_CHECK(woke::util::nearly_equal(woke::util::scale_rgb(woke::util::from_hex(0x808080), 2.0f).r, 1.0f)); // clamps
    WOKE_CHECK(woke::util::kTransparent.a == 0.0f);

    // ── easing ────────────────────────────────────────────────────────────────────
    woke_test::section("ui easing curves");
    // Both endpoints exact: a curve that returns 0.999 at t = 1 leaves a widget one pixel short.
    WOKE_CHECK(ease_out_cubic(0.0f) == 0.0f);
    WOKE_CHECK(ease_out_cubic(1.0f) == 1.0f);
    WOKE_CHECK(ease_out_quint(0.0f) == 0.0f);
    WOKE_CHECK(ease_out_quint(1.0f) == 1.0f);
    WOKE_CHECK(ease_in_out_cubic(0.0f) == 0.0f);
    WOKE_CHECK(ease_in_out_cubic(1.0f) == 1.0f);
    WOKE_CHECK(ease_in_out_cubic(0.5f) == 0.5f);
    WOKE_CHECK(ease_out_expo(0.0f) == 0.0f);
    WOKE_CHECK(ease_out_expo(1.0f) == 1.0f);
    WOKE_CHECK(ease_out_expo(2.0f) == 1.0f);

    // The documented shape: easeOutCubic is fast early, and every curve is monotonic.
    WOKE_CHECK(woke::util::nearly_equal(ease_out_cubic(0.5f), 0.875f));
    WOKE_CHECK(ease_out_expo(0.25f) > ease_out_cubic(0.25f));
    float previous = -1.0f;
    for (int step = 0; step <= 20; ++step) {
        const float t = static_cast<float>(step) / 20.0f;
        const float value = ease_out_expo(t);
        WOKE_CHECK(value >= previous);
        WOKE_CHECK(value >= 0.0f && value <= 1.0f);
        previous = value;
    }
    WOKE_CHECK(ease_out_back(0.7f) > 1.0f); // deliberate overshoot exist

    // ── animation controller ──────────────────────────────────────────────────────
    woke_test::section("ui animation controller");
    AnimationController controller;
    const StateHandle hover = controller.acquire_state(0.0f, 14.0f);
    WOKE_CHECK(hover.valid());
    WOKE_CHECK(controller.valid(hover));
    WOKE_CHECK(controller.value(hover) == 0.0f);
    WOKE_CHECK(controller.active_state_count() == 1);

    controller.set_target(hover, 1.0f);
    WOKE_CHECK(controller.target(hover) == 1.0f);
    WOKE_CHECK(!controller.settled(hover));
    controller.tick(0.05f);
    const float early = controller.value(hover);
    WOKE_CHECK(early > 0.0f && early < 1.0f);
    for (int step = 0; step < 100; ++step) {
        controller.tick(0.05f);
    }
    WOKE_CHECK(controller.value(hover) == 1.0f); // settles exactly, not asymptotically close
    WOKE_CHECK(controller.settled(hover));

    // Delta time, not frame count: 1/60 s once == 1/240 s four times.
    WOKE_CHECK(woke::util::nearly_equal(
        advance_once(1.0f / 60.0f), advance_many(1.0f / 60.0f, 4), 0.0002f));
    WOKE_CHECK(woke::util::nearly_equal(
        advance_once(0.5f), advance_many(0.5f, 120), 0.0002f));

    // A non-positive delta cannot rewind an animation.
    const float frozen = controller.value(hover);
    controller.tick(0.0f);
    controller.tick(-1.0f);
    WOKE_CHECK(controller.value(hover) == frozen);

    // A recycled slot must not be reachable through the handle that used to own it.
    const StateHandle stale = hover;
    controller.release(hover);
    WOKE_CHECK(!controller.valid(stale));
    WOKE_CHECK(controller.active_state_count() == 0);
    const StateHandle reused = controller.acquire_state(0.5f, 14.0f);
    WOKE_CHECK(controller.valid(reused));
    WOKE_CHECK(controller.value(stale) == 0.0f); // the stale handle reads nothing
    controller.set_target(stale, 9.0f);          // and writes nothing
    WOKE_CHECK(controller.target(reused) == 0.5f);
    controller.set_value(reused, 0.2f);
    WOKE_CHECK(controller.value(reused) == 0.2f);
    controller.snap(reused, 0.7f);
    WOKE_CHECK(controller.value(reused) == 0.7f && controller.target(reused) == 0.7f);

    // Overflow is reported rather than silently dropping an animation.
    AnimationController full;
    std::size_t acquired = 0;
    for (std::size_t index = 0; index < AnimationController::kMaxStates; ++index) {
        if (full.acquire_state().valid()) {
            ++acquired;
        }
    }
    WOKE_CHECK(acquired == AnimationController::kMaxStates);
    WOKE_CHECK(!full.acquire_state().valid());
    WOKE_CHECK(full.overflow_count() == 1);
    full.reset();
    WOKE_CHECK(full.active_state_count() == 0);
    WOKE_CHECK(full.acquire_state().valid());

    // Springs: the GUI open/close, plus the frame-delta clamp the scheduler applies.
    const SpringHandle open = controller.acquire_spring(0.0f);
    WOKE_CHECK(open.valid());
    WOKE_CHECK(controller.at_rest(open));
    controller.set_target(open, 1.0f);
    WOKE_CHECK(!controller.at_rest(open));
    controller.tick(1.0f / 60.0f);
    const float partway = controller.value(open);
    WOKE_CHECK(partway > 0.0f && partway < 1.0f);
    for (int step = 0; step < 240; ++step) {
        controller.tick(1.0f / 60.0f);
    }
    WOKE_CHECK(controller.value(open) == 1.0f);
    WOKE_CHECK(controller.at_rest(open));

    // A hitch must not launch the window off screen: substepping keeps a 0.25 s delta bounded.
    const SpringHandle hitch = controller.acquire_spring(0.0f);
    controller.set_target(hitch, 1.0f);
    controller.tick(0.25f);
    const float after_hitch = controller.value(hitch);
    WOKE_CHECK(after_hitch >= 0.0f && after_hitch <= 1.0f);
    controller.snap(hitch, 0.0f);
    WOKE_CHECK(controller.value(hitch) == 0.0f);
    WOKE_CHECK(controller.valid(hitch));
    controller.release(hitch);
    WOKE_CHECK(!controller.valid(hitch));
    controller.release(SpringHandle{}); // a default handle is harmless

    // ── theme tokens ──────────────────────────────────────────────────────────────
    woke_test::section("ui theme tokens");
    WOKE_CHECK(woke::ui::kSectionCount == 12);
    WOKE_CHECK(woke::ui::kModuleCategoryCount == 6);
    WOKE_CHECK(count_modules_in_catalog() == static_cast<int>(kCatalogTotal));

    for (std::size_t index = 0; index < woke::ui::kSectionCount; ++index) {
        const auto section = static_cast<woke::ui::Section>(index);
        const char* label = woke::ui::section_label(section);
        const char* subtitle = woke::ui::section_subtitle(section);
        WOKE_CHECK(label != nullptr && label[0] != '\0');
        WOKE_CHECK(subtitle != nullptr && subtitle[0] != '\0');
        if (index < woke::ui::kModuleCategoryCount) {
            WOKE_CHECK(woke::ui::is_module_category(section));
            WOKE_CHECK(woke::ui::category_catalog_size(section) > 0);
        } else {
            WOKE_CHECK(!woke::ui::is_module_category(section));
            WOKE_CHECK(woke::ui::category_catalog_size(section) == 0);
        }
    }
    WOKE_CHECK(woke::ui::section_label(woke::ui::Section::Combat)[0] == 'C');
    WOKE_CHECK(woke::ui::section_label(woke::ui::Section::Diagnostics)[0] == 'D');

    // §7.2's traffic-light column, exactly.
    const Rgba red = woke::ui::theme::traffic_light(0);
    const Rgba yellow = woke::ui::theme::traffic_light(1);
    const Rgba green = woke::ui::theme::traffic_light(2);
    WOKE_CHECK(red == woke::util::from_hex(0xFF5F56));
    WOKE_CHECK(yellow == woke::util::from_hex(0xFFBD2E));
    WOKE_CHECK(green == woke::util::from_hex(0x27C93F));
    WOKE_CHECK(woke::ui::theme::traffic_light(7) == woke::ui::theme::color::kTextDim);

    // The style table is derived from the metrics, so the two cannot drift apart.
    const woke::ui::theme::StyleTokens tokens = woke::ui::theme::style_tokens();
    WOKE_CHECK(tokens.window_rounding == woke::ui::theme::metrics::kWindowRounding);
    WOKE_CHECK(tokens.frame_rounding == woke::ui::theme::metrics::kFrameRounding);
    WOKE_CHECK(tokens.window_border_size == woke::ui::theme::metrics::kWindowBorderSize);
    WOKE_CHECK(tokens.window_rounding > tokens.frame_rounding);
    WOKE_CHECK(woke::ui::theme::color::kWindowBackdrop.a == 0.90f);

    // Every category gets its own accent, and none of them is the muted fallback.
    for (std::size_t index = 0; index < woke::ui::kModuleCategoryCount; ++index) {
        const auto section = static_cast<woke::ui::Section>(index);
        const Rgba accent = woke::ui::theme::category_accent(section);
        WOKE_CHECK(!(accent == woke::ui::theme::color::kTextMuted));
        WOKE_CHECK(accent.a == 1.0f);
    }
    WOKE_CHECK(woke::ui::theme::category_accent(woke::ui::Section::Combat)
        == woke::util::from_hex(0xFF6B6B));
    WOKE_CHECK(woke::ui::theme::category_accent(woke::ui::Section::Diagnostics)
        == woke::ui::theme::color::kTextMuted);
}
