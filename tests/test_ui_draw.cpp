#include "test_harness.h"

// Draw-layer tests (roadmap step 4).
//
// ImGui's core is platform independent, so the chrome's geometry and drawing can be exercised
// headlessly on Linux: a context with a display size, one NewFrame/Render pair, and assertions
// about the draw data that comes out. That covers the two things most likely to be wrong - the
// primitive helpers and the input/allocation logic of a component - without a game, a GL context or
// a screenshot. What it cannot cover is how it looks; that is the in-game gate.

#include <imgui.h>

#include "ui/animation/animation_controller.h"
#include "ui/components/traffic_lights.h"
#include "ui/theme.h"
#include "utils/render_utils.h"

namespace {

// A namespace alias rather than a using-directive: the draw layer is referred to as `draw::`
// everywhere in this file, exactly as the composition code does.
namespace draw = woke::ui::draw;

using draw::Rect;
using woke::ui::animation::AnimationController;
using woke::ui::components::Input;
using woke::ui::components::TrafficLights;

int total_vertices() {
    const ImDrawData* data = ImGui::GetDrawData();
    return data != nullptr ? data->TotalVtxCount : 0;
}

// ImDrawData::CmdListsCount only exists while IMGUI_DISABLE_OBSOLETE_FUNCTIONS is off, and the
// vendored build defines it on, so the list size is the portable read.
int total_command_lists() {
    const ImDrawData* data = ImGui::GetDrawData();
    return data != nullptr ? data->CmdLists.Size : 0;
}

void press(Input& input, float x, float y) {
    input.mouse_x = x;
    input.mouse_y = y;
    input.down[0] = true;
    input.pressed[0] = true;
    input.released[0] = false;
}

void release(Input& input, float x, float y) {
    input.mouse_x = x;
    input.mouse_y = y;
    input.down[0] = false;
    input.pressed[0] = false;
    input.released[0] = true;
}

void hover(Input& input, float x, float y) {
    input.mouse_x = x;
    input.mouse_y = y;
    input.down[0] = false;
    input.pressed[0] = false;
    input.released[0] = false;
}

// A context with a display size and no platform backend: enough for ImGui::NewFrame, widget layout
// and Render() to produce draw data. `RendererHasTextures` is what the OpenGL3 backend advertises
// too, and without it ImGui asserts that some backend uploaded the font atlas.
void begin_headless_frame(float width, float height) {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = ImVec2(width, height);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->AddFontDefault();
    ImGui::NewFrame();
}

} // namespace

void test_ui_draw() {
    // ── Rect: the layout math the composition is written in ───────────────────────
    woke_test::section("ui draw geometry");
    const Rect window{100.0f, 200.0f, 980.0f, 620.0f};
    WOKE_CHECK(window.left() == 100.0f);
    WOKE_CHECK(window.top() == 200.0f);
    WOKE_CHECK(window.right() == 1080.0f);
    WOKE_CHECK(window.bottom() == 820.0f);
    WOKE_CHECK(window.center_x() == 590.0f);
    WOKE_CHECK(window.contains(500.0f, 500.0f));
    WOKE_CHECK(!window.contains(50.0f, 500.0f));
    WOKE_CHECK(Rect{}.contains(0.0f, 0.0f)); // a zero-size rect still contains its origin
    WOKE_CHECK(Rect{}.empty());
    WOKE_CHECK(window.inset(20.0f).w == 940.0f);
    WOKE_CHECK(window.offset(10.0f, -10.0f).top() == 190.0f);

    const Rect header = window.slice_top(64.0f);
    WOKE_CHECK(header.h == 64.0f);
    const Rect body = window.without_top(header.h);
    WOKE_CHECK(body.top() == 264.0f);
    WOKE_CHECK(body.h == 556.0f);
    WOKE_CHECK(body.bottom() == window.bottom());
    const Rect rail = body.slice_left(220.0f);
    WOKE_CHECK(rail.w == 220.0f);
    WOKE_CHECK(rail.h == body.h);
    const Rect content = body.without_left(rail.w);
    WOKE_CHECK(content.left() == rail.right());
    WOKE_CHECK(content.w == body.w - rail.w);
    WOKE_CHECK(body.below(20.0f).top() == body.bottom());

    // ── Colour conversion ─────────────────────────────────────────────────────────
    WOKE_CHECK(draw::to_im_u32(woke::util::kWhite) == IM_COL32(255, 255, 255, 255));
    WOKE_CHECK(draw::to_im_u32(woke::util::kBlack) == IM_COL32(0, 0, 0, 255));
    WOKE_CHECK(draw::to_im_u32(woke::util::kTransparent) == IM_COL32(0, 0, 0, 0));
    const ImVec4 vec = draw::to_im_vec4(woke::ui::theme::color::kAccent);
    WOKE_CHECK(woke::util::nearly_equal(vec.w, 1.0f));

    // ── Primitives, inside a real (headless) ImGui frame ──────────────────────────
    woke_test::section("ui draw primitives");
    begin_headless_frame(1280.0f, 720.0f);
    WOKE_CHECK(ImGui::GetCurrentContext() != nullptr);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(1280.0f, 720.0f));
    const bool window_ok = ImGui::Begin("draw-test", nullptr, ImGuiWindowFlags_NoDecoration);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    WOKE_CHECK(window_ok);
    WOKE_CHECK(draw_list != nullptr);

    ImGui::Dummy(ImVec2(1.0f, 1.0f));
    const Rect area{40.0f, 40.0f, 400.0f, 200.0f};

    // Every helper must be a no-op on a null list or an empty rectangle, because the composition
    // calls them with conditions it has already checked elsewhere.
    draw::rounded_rect(nullptr, area, woke::util::kWhite, 8.0f);
    draw::rounded_rect(draw_list, Rect{}, woke::util::kWhite, 8.0f);
    draw::border_stroke(nullptr, area, woke::util::kWhite, 8.0f);
    draw::shadow_rect(nullptr, area, 12.0f);
    draw::shadow_rect(draw_list, Rect{}, 12.0f);
    draw::gradient_fill_v(nullptr, area, woke::util::kWhite, woke::util::kBlack);
    draw::gradient_fill_h(draw_list, Rect{}, woke::util::kWhite, woke::util::kBlack);
    draw::circle(nullptr, 0.0f, 0.0f, 4.0f, woke::util::kWhite);
    draw::ring(draw_list, 0.0f, 0.0f, 0.0f, woke::util::kWhite);
    draw::line(nullptr, 0.0f, 0.0f, 1.0f, 1.0f, woke::util::kWhite);
    draw::text(nullptr, 0.0f, 0.0f, "x", woke::util::kWhite);
    draw::text_in(draw_list, Rect{}, "x", woke::util::kWhite);
    draw::text_clipped(draw_list, Rect{}, "x", woke::util::kWhite);

    draw::rounded_rect(draw_list, area, woke::ui::theme::color::kCard, 8.0f);
    draw::border_stroke(draw_list, area, woke::ui::theme::color::kCardBorder, 8.0f);
    draw::shadow_rect(draw_list, area, 12.0f);
    draw::gradient_fill_v(draw_list, area, woke::ui::theme::color::kCard,
        woke::ui::theme::color::kCardHover);
    draw::gradient_fill_h(draw_list, area, woke::ui::theme::color::kCard,
        woke::ui::theme::color::kCardHover);
    draw::circle(draw_list, 100.0f, 100.0f, 6.0f, woke::ui::theme::color::kTrafficRed);
    draw::ring(draw_list, 100.0f, 100.0f, 8.0f, woke::util::kWhite);
    draw::line(draw_list, 0.0f, 0.0f, 40.0f, 40.0f, woke::ui::theme::color::kSeparator);
    draw::text(draw_list, 10.0f, 10.0f, "woke.wtf", woke::util::kWhite);
    draw::text_in(draw_list, area, "centered", woke::util::kWhite, draw::Align::Center);
    draw::text_in(draw_list, area, "right", woke::util::kWhite, draw::Align::Right,
        woke::ui::theme::metrics::kHeaderFontSize);
    draw::text_clipped(draw_list, Rect{0.0f, 0.0f, 28.0f, 16.0f},
        "a label that cannot possibly fit in this box", woke::util::kWhite);

    // A scaled heading and a measured string: the two paths the chrome bar uses.
    WOKE_CHECK(draw::text_height(woke::ui::theme::metrics::kTitleFontSize)
        == woke::ui::theme::metrics::kTitleFontSize);
    WOKE_CHECK(draw::text_height() > 0.0f);
    WOKE_CHECK(draw::text_width("woke.wtf") > 0.0f);
    WOKE_CHECK(draw::text_width("") == 0.0f);
    WOKE_CHECK(draw::text_width("woke.wtf", 26.0f) > draw::text_width("woke.wtf", 13.0f));

    ImGui::End();
    ImGui::Render();
    WOKE_CHECK(total_vertices() > 0);
    WOKE_CHECK(total_command_lists() > 0);

    ImGui::DestroyContext();
    WOKE_CHECK(ImGui::GetCurrentContext() == nullptr);

    // ── The traffic-light component ───────────────────────────────────────────────
    woke_test::section("ui traffic lights");
    AnimationController animation;
    TrafficLights lights;
    lights.bind(animation);

    const Rect strip{100.0f, 50.0f, 60.0f, 40.0f};
    lights.set_area(strip);

    const Rect close = lights.button_rect(strip, 0);
    const Rect minimize = lights.button_rect(strip, 1);
    const Rect zoom = lights.button_rect(strip, 2);
    WOKE_CHECK(close.w == 12.0f); // Ø 12 per §7.2
    WOKE_CHECK(close.center_y() == strip.center_y());
    WOKE_CHECK(minimize.center_x() - close.center_x() == 20.0f); // 20 px centre spacing
    WOKE_CHECK(zoom.center_x() - minimize.center_x() == 20.0f);
    WOKE_CHECK(close.center_x() < minimize.center_x());

    // A press that starts outside a button is not ours.
    Input input;
    press(input, 400.0f, 400.0f);
    WOKE_CHECK(!lights.handle_input(input));
    WOKE_CHECK(lights.take_action() == TrafficLights::Action::None);

    // Close: press and release on the red button.
    press(input, close.center_x(), close.center_y());
    WOKE_CHECK(lights.handle_input(input)); // a press on a button consumes the click
    // The component steers its targets in animate(); the GUI owns the single controller tick.
    lights.animate(1.0f / 60.0f);
    animation.tick(1.0f / 60.0f);
    WOKE_CHECK(lights.press_amount(0) > 0.0f);
    WOKE_CHECK(lights.hover_amount(0) > 0.0f);
    WOKE_CHECK(lights.hover_amount(1) == 0.0f);

    release(input, close.center_x(), close.center_y());
    WOKE_CHECK(lights.handle_input(input));
    WOKE_CHECK(lights.take_action() == TrafficLights::Action::Close);
    WOKE_CHECK(lights.take_action() == TrafficLights::Action::None); // consumed once

    // Minimize and zoom map to the other two buttons.
    press(input, minimize.center_x(), minimize.center_y());
    WOKE_CHECK(lights.handle_input(input));
    release(input, minimize.center_x(), minimize.center_y());
    WOKE_CHECK(lights.handle_input(input));
    WOKE_CHECK(lights.take_action() == TrafficLights::Action::Minimize);

    press(input, zoom.center_x(), zoom.center_y());
    WOKE_CHECK(lights.handle_input(input));
    release(input, zoom.center_x(), zoom.center_y());
    WOKE_CHECK(lights.handle_input(input));
    WOKE_CHECK(lights.take_action() == TrafficLights::Action::Zoom);

    // Dragging off a button cancels it: no action, and the press is released.
    press(input, close.center_x(), close.center_y());
    (void)lights.handle_input(input);
    release(input, 800.0f, 600.0f);
    WOKE_CHECK(!lights.handle_input(input));
    WOKE_CHECK(lights.take_action() == TrafficLights::Action::None);

    // Hover is per button and never consumes input on its own.
    hover(input, zoom.center_x(), zoom.center_y());
    WOKE_CHECK(!lights.handle_input(input));
    for (int step = 0; step < 30; ++step) {
        lights.animate(1.0f / 60.0f);
        animation.tick(1.0f / 60.0f);
    }
    WOKE_CHECK(lights.hover_amount(2) > 0.9f);
    WOKE_CHECK(lights.hover_amount(0) < 0.05f);

    // Drawing the component produces geometry, and the pooled animation slots come back on unbind.
    begin_headless_frame(800.0f, 600.0f);
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(800.0f, 600.0f));
    ImGui::Begin("lights-test", nullptr, ImGuiWindowFlags_NoDecoration);
    ImGui::Dummy(ImVec2(1.0f, 1.0f));

    // The component must put geometry into the list it is handed: no text, no layout, just the
    // circles (and the hover ring and glyphs the state above left active).
    ImDrawList* lights_list = ImGui::GetWindowDrawList();
    const int vertices_before = lights_list->VtxBuffer.Size;
    lights.render(lights_list, strip);
    WOKE_CHECK(lights_list->VtxBuffer.Size > vertices_before);

    ImGui::End();
    ImGui::Render();
    WOKE_CHECK(total_vertices() > 0);
    ImGui::DestroyContext();

    const std::size_t in_use = animation.active_state_count();
    WOKE_CHECK(in_use == TrafficLights::kCount * 2); // hover + press per button
    lights.unbind();
    WOKE_CHECK(animation.active_state_count() == 0);
    WOKE_CHECK(lights.hover_amount(0) == 0.0f);
    WOKE_CHECK(lights.press_amount(2) == 0.0f);
    lights.unbind(); // idempotent
}
