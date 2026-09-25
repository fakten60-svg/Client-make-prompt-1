#include "test_harness.h"

// Visual-module tests (roadmap step 7).
//
// The step's gate is "Fullbright/HUD/Zoom/Trajectories behave in singleplayer", which needs a
// game; what runs here is everything the game is not needed for:
//
//   * the projectile integration and the world-to-screen projection (pure math),
//   * the crosshair geometry (pure math),
//   * the client-write seam, including that a toggle captures the baseline and puts it back,
//   * the three overlay modules' enable/disable behaviour through that seam, and
//   * the overlay policy and the HUD renderer against a headless ImGui draw list.
//
// What this cannot cover is how any of it looks and whether the gamma write lands in a live
// client; that is the manual in-game part of the gate (§12.2).

#include <array>
#include <cmath>

#include <imgui.h>

#include "modules/game_writes.h"
#include "modules/module_manager.h"
#include "modules/visual/crosshair_geometry.h"
#include "modules/visual/custom_crosshair.h"
#include "modules/visual/fullbright.h"
#include "modules/visual/hud_module.h"
#include "modules/visual/trajectories.h"
#include "modules/visual/trajectory.h"
#include "modules/visual/zoom.h"
#include "ui/hud.h"
#include "ui/overlay.h"

namespace {

namespace visual = woke::modules::visual;
namespace hud = woke::ui::hud;
namespace draw = woke::ui::draw;

[[nodiscard]] bool nearly(float a, float b, float epsilon = 0.02f) noexcept {
    return (a > b ? a - b : b - a) <= epsilon;
}

// A headless frame handing out a real draw list, the same shape the widget tests use: components
// only ever need one, and ImGui insists on a window to produce it.
struct HeadlessFrame {
    ImDrawList* list = nullptr;
    int vertices = 0;

    HeadlessFrame() {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DisplaySize = ImVec2(1280.0f, 720.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontDefault();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(1280.0f, 720.0f));
        ImGui::Begin("hud-test", nullptr, ImGuiWindowFlags_NoDecoration);
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
        list = ImGui::GetWindowDrawList();
        vertices = list->VtxBuffer.Size;
    }

    ~HeadlessFrame() {
        ImGui::End();
        ImGui::Render();
        ImGui::DestroyContext();
    }

    [[nodiscard]] int vertices_added() const noexcept { return list->VtxBuffer.Size - vertices; }
};

void test_trajectory_math() {
    woke_test::section("trajectory prediction");

    std::array<woke::game::Vec3d, visual::kMaxTrajectoryPoints> path{};
    visual::TrajectoryParams params{};
    params.gravity = 0.05;
    params.drag = 0.99;
    params.seconds = 2.0; // 40 samples at the game's 20 Hz step

    const woke::game::Vec3d origin{0.0, 64.0, 0.0};
    const woke::game::Vec3d velocity{0.0, 0.3, 1.0};
    const std::size_t count = visual::predict_trajectory(origin, velocity, params, path);
    WOKE_CHECK(count == 40);

    // A projectile thrown forward and up rises, then falls: the peak must be inside the path and
    // the last sample must be below the origin's y once gravity has won.
    bool peaked = false;
    for (std::size_t index = 0; index < count; ++index) {
        if (path[index].y > origin.y) {
            peaked = true;
        }
        // +Z drift is monotonic and no sample ever moves backwards.
        if (index > 0) {
            WOKE_CHECK(path[index].z > path[index - 1].z);
        }
    }
    WOKE_CHECK(peaked);
    WOKE_CHECK(path[count - 1].y < origin.y);

    // A zero-second horizon and a zero-tick rate must both be survivable, not a division or a
    // loop bound that runs away.
    visual::TrajectoryParams degenerate{};
    degenerate.seconds = 0.0;
    degenerate.ticks_per_second = 0;
    WOKE_CHECK(visual::predict_trajectory(origin, velocity, degenerate, path) == 0);

    // The buffer is a hard ceiling: asking for more seconds than it can hold clips, never grows.
    visual::TrajectoryParams long_run{};
    long_run.seconds = 60.0;
    WOKE_CHECK(visual::predict_trajectory(origin, velocity, long_run, path) == path.size());
}

void test_projection() {
    woke_test::section("world-to-screen projection");

    visual::View view{};
    view.eye = woke::game::Vec3d{0.0, 0.0, 0.0};
    view.yaw_degrees = 0.0f; // looking toward +Z
    view.pitch_degrees = 0.0f;
    view.vertical_fov_degrees = 70.0f;
    view.viewport_width = 1280.0f;
    view.viewport_height = 720.0f;

    visual::ScreenPoint point{};
    // Dead ahead: the centre of the viewport.
    WOKE_CHECK(visual::project_to_screen(view, woke::game::Vec3d{0.0, 0.0, 10.0}, point));
    WOKE_CHECK(nearly(point.x, 640.0f));
    WOKE_CHECK(nearly(point.y, 360.0f));

    // To the right and above: right of centre and above centre (screen y grows downward).
    WOKE_CHECK(visual::project_to_screen(view, woke::game::Vec3d{5.0, 5.0, 10.0}, point));
    WOKE_CHECK(point.x > 640.0f);
    WOKE_CHECK(point.y < 360.0f);

    // Behind the camera: refused rather than wrapped around the screen.
    WOKE_CHECK(!visual::project_to_screen(view, woke::game::Vec3d{0.0, 0.0, -10.0}, point));

    // Turning 180 degrees brings that same point back into view.
    view.yaw_degrees = 180.0f;
    WOKE_CHECK(visual::project_to_screen(view, woke::game::Vec3d{0.0, 0.0, -10.0}, point));
    WOKE_CHECK(nearly(point.x, 640.0f));

    // A narrower field of view magnifies: the same offset projects farther from the centre.
    view.yaw_degrees = 0.0f;
    visual::ScreenPoint zoomed{};
    WOKE_CHECK(visual::project_to_screen(view, woke::game::Vec3d{5.0, 0.0, 10.0}, point));
    view.vertical_fov_degrees = 35.0f;
    WOKE_CHECK(visual::project_to_screen(view, woke::game::Vec3d{5.0, 0.0, 10.0}, zoomed));
    WOKE_CHECK(zoomed.x > point.x);
}

void test_crosshair_geometry() {
    woke_test::section("crosshair geometry");

    const visual::CrosshairGeometry cross =
        visual::crosshair_geometry(visual::CrosshairShape::Cross, 8.0f, 3.0f, 1.5f);
    WOKE_CHECK(cross.arm_count == 4);
    WOKE_CHECK(!cross.dot);
    WOKE_CHECK(!cross.ring);
    WOKE_CHECK(nearly(cross.thickness, 1.5f));
    // Left arm: from -(gap + size) to -gap. The gap is the whole point of the shape.
    WOKE_CHECK(nearly(cross.arms[0].x0, -11.0f));
    WOKE_CHECK(nearly(cross.arms[0].x1, -3.0f));
    WOKE_CHECK(nearly(cross.arms[0].y0, 0.0f));
    // Right arm mirrors it.
    WOKE_CHECK(nearly(cross.arms[1].x0, 3.0f));
    WOKE_CHECK(nearly(cross.arms[1].x1, 11.0f));

    const visual::CrosshairGeometry dot =
        visual::crosshair_geometry(visual::CrosshairShape::Dot, 8.0f, 3.0f, 2.0f);
    WOKE_CHECK(dot.arm_count == 0);
    WOKE_CHECK(dot.dot);

    const visual::CrosshairGeometry both =
        visual::crosshair_geometry(visual::CrosshairShape::CrossDot, 6.0f, 0.0f, 1.0f);
    WOKE_CHECK(both.arm_count == 4);
    WOKE_CHECK(both.dot);
    WOKE_CHECK(nearly(both.arms[0].x1, 0.0f)); // no gap means the arms meet the centre

    const visual::CrosshairGeometry ring =
        visual::crosshair_geometry(visual::CrosshairShape::Circle, 10.0f, 2.0f, 1.0f);
    WOKE_CHECK(ring.arm_count == 0);
    WOKE_CHECK(ring.ring);
    WOKE_CHECK(nearly(ring.ring_radius, 7.0f));

    // Degenerate settings are clamped rather than producing a zero-length or negative shape.
    const visual::CrosshairGeometry clamped =
        visual::crosshair_geometry(visual::CrosshairShape::Cross, -5.0f, -5.0f, 0.0f);
    WOKE_CHECK(nearly(clamped.thickness, 0.5f));
    WOKE_CHECK(nearly(clamped.arms[1].x1, 1.0f)); // minimum arm length
}

void test_write_seam() {
    woke_test::section("client write seam");

    woke::modules::RecordingGameWrites recorder;
    recorder.seed(0.5f, 70.0f);

    woke::modules::GameWrites* previous = woke::modules::set_game_writes(&recorder);
    WOKE_CHECK(previous != nullptr);

    const woke::game::Maybe<float> gamma_before = woke::modules::game_writes().gamma();
    WOKE_CHECK(gamma_before.valid && nearly(gamma_before.value, 0.5f));

    // The first apply captures the live value as the baseline; restore puts that value back.
    WOKE_CHECK(woke::modules::game_writes().apply_gamma(1.0f));
    WOKE_CHECK(recorder.gamma_active());
    WOKE_CHECK(nearly(recorder.gamma_value(), 1.0f));
    WOKE_CHECK(nearly(recorder.baseline_gamma(), 0.5f));
    woke::modules::game_writes().restore_gamma();
    WOKE_CHECK(!recorder.gamma_active());
    WOKE_CHECK(nearly(recorder.gamma_value(), 0.5f));

    WOKE_CHECK(woke::modules::game_writes().apply_fov(30.0f));
    WOKE_CHECK(nearly(recorder.baseline_fov(), 70.0f));
    woke::modules::game_writes().restore_fov();
    WOKE_CHECK(nearly(recorder.fov_value(), 70.0f));

    // A backend that reports the option as unreachable makes both reads and writes fail honestly.
    recorder.set_available(false);
    WOKE_CHECK(!woke::modules::game_writes().gamma().valid);
    WOKE_CHECK(!woke::modules::game_writes().apply_gamma(1.0f));
    recorder.set_available(true);

    // Restoring the default returns the recorder that was installed.
    WOKE_CHECK(woke::modules::set_game_writes(nullptr) == &recorder);
}

void test_write_modules() {
    woke_test::section("fullbright and zoom modules");

    visual::Fullbright fullbright;
    WOKE_CHECK(fullbright.setting_count() == 1);
    WOKE_CHECK_STR(fullbright.name(), "Fullbright");
    WOKE_CHECK(fullbright.category() == woke::modules::Category::Visual);

    visual::Zoom zoom;
    WOKE_CHECK(zoom.setting_count() == 2);
    WOKE_CHECK(zoom.bind() == 0x43);

    woke::modules::RecordingGameWrites recorder;
    recorder.seed(0.4f, 80.0f);
    (void)woke::modules::set_game_writes(&recorder);

    // ── Fullbright: applies on enable, follows the slider, restores on disable ──
    WOKE_CHECK(fullbright.enable());
    WOKE_CHECK(recorder.gamma_active());
    WOKE_CHECK(nearly(recorder.gamma_value(), 1.0f)); // the default brightness
    WOKE_CHECK(nearly(recorder.baseline_gamma(), 0.4f));

    const std::size_t applies_after_enable = recorder.gamma_applies();
    fullbright.on_tick(0.05f);
    WOKE_CHECK(recorder.gamma_applies() == applies_after_enable); // unchanged value: no write

    fullbright.settings()[0]->set_float(0.25f);
    fullbright.on_tick(0.05f);
    WOKE_CHECK(nearly(recorder.gamma_value(), 0.25f));
    WOKE_CHECK(recorder.gamma_applies() == applies_after_enable + 1);

    WOKE_CHECK(fullbright.disable());
    WOKE_CHECK(!recorder.gamma_active());
    WOKE_CHECK(nearly(recorder.gamma_value(), 0.4f));

    // ── Zoom: narrows the field of view toward base/factor and restores it ──
    WOKE_CHECK(zoom.enable());
    WOKE_CHECK(recorder.fov_active());
    WOKE_CHECK(nearly(recorder.baseline_fov(), 80.0f));
    WOKE_CHECK(nearly(recorder.fov_value(), 80.0f)); // the enable writes the current value first

    for (int tick = 0; tick < 60; ++tick) {
        zoom.on_tick(0.05f);
    }
    // factor defaults to 3.0, so the settled field of view is 80 / 3.
    WOKE_CHECK(nearly(recorder.fov_value(), 80.0f / 3.0f, 0.2f));
    WOKE_CHECK(recorder.fov_value() < 80.0f);

    WOKE_CHECK(zoom.disable());
    WOKE_CHECK(!recorder.fov_active());
    WOKE_CHECK(nearly(recorder.fov_value(), 80.0f));

    // ── Unreachable options: the module refuses to enable rather than lying ──
    recorder.set_available(false);
    visual::Fullbright offline_fullbright;
    WOKE_CHECK(!offline_fullbright.enable());
    WOKE_CHECK(!offline_fullbright.enabled());
    visual::Zoom offline_zoom;
    WOKE_CHECK(!offline_zoom.enable());
    WOKE_CHECK(!offline_zoom.enabled());

    (void)woke::modules::set_game_writes(nullptr);
}

void test_overlay_modules_and_hud() {
    woke_test::section("overlay modules, policy and renderer");

    // A registry holding exactly the three overlay modules, plus one unrelated module so the
    // arraylist has something truthful to refuse to draw for.
    woke::modules::visual::HudModule hud_module;
    woke::modules::visual::CustomCrosshair crosshair;
    woke::modules::visual::Trajectories trajectories;
    visual::Fullbright unrelated;

    woke::modules::ModuleManager& registry = woke::modules::manager();
    registry.reset();
    WOKE_CHECK(registry.add(&hud_module));
    WOKE_CHECK(registry.add(&crosshair));
    WOKE_CHECK(registry.add(&trajectories));
    WOKE_CHECK(registry.add(&unrelated));

    // ── Policy: nothing enabled means nothing wants a frame ──
    WOKE_CHECK(!woke::ui::overlay::wanted());
    WOKE_CHECK(!hud_module.draws());

    WOKE_CHECK(hud_module.enable());
    WOKE_CHECK(hud_module.draws());
    WOKE_CHECK(woke::ui::overlay::wanted());

    // Turning both HUD outputs off makes the module enabled but silent.
    hud_module.settings()[0]->set_bool(false); // watermark
    hud_module.settings()[1]->set_bool(false); // arraylist
    WOKE_CHECK(!hud_module.draws());
    WOKE_CHECK(!woke::ui::overlay::wanted());
    WOKE_CHECK(hud_module.enable() == false); // still enabled: nothing to change

    WOKE_CHECK(crosshair.enable());
    WOKE_CHECK(woke::ui::overlay::wanted());
    WOKE_CHECK(crosshair.disable());

    WOKE_CHECK(trajectories.enable());
    WOKE_CHECK(woke::ui::overlay::wanted());
    WOKE_CHECK(trajectories.disable());

    // The unrelated module must never make the overlay claim it has work.
    WOKE_CHECK(unrelated.enable());
    WOKE_CHECK(!woke::ui::overlay::wanted());
    WOKE_CHECK(unrelated.disable());

    // ── Renderer: a frame with only the HUD on draws something, and an empty frame draws nothing ──
    hud_module.settings()[0]->set_bool(true);
    hud_module.settings()[1]->set_bool(true);

    hud::Frame frame{};
    frame.screen = draw::Rect{0.0f, 0.0f, 1280.0f, 720.0f};
    WOKE_CHECK(!hud::active(frame));
    {
        HeadlessFrame headless;
        hud::render(headless.list, frame);
        WOKE_CHECK(headless.vertices_added() == 0);
    }

    frame.watermark = true;
    frame.arraylist = true;
    frame.frames_per_second = 240.0f;
    WOKE_CHECK(hud::active(frame));
    {
        HeadlessFrame headless;
        hud::render(headless.list, frame);
        // The watermark plate, its accent strip and its text, plus a row per enabled module.
        WOKE_CHECK(headless.vertices_added() > 0);
    }

    // ── Renderer: the crosshair emits geometry for each shape ──
    hud::Frame cross_frame{};
    cross_frame.screen = frame.screen;
    cross_frame.crosshair = true;
    cross_frame.crosshair_style.shape = visual::CrosshairShape::CrossDot;
    cross_frame.crosshair_style.color = woke::modules::visual::CustomCrosshair::color_for(0);
    cross_frame.crosshair_style.size = 8.0f;
    cross_frame.crosshair_style.gap = 3.0f;
    cross_frame.crosshair_style.thickness = 1.5f;
    WOKE_CHECK(hud::active(cross_frame));
    {
        HeadlessFrame headless;
        hud::render(headless.list, cross_frame);
        WOKE_CHECK(headless.vertices_added() > 0);
    }

    // ── Renderer: the trajectory projection produces usable screen points ──
    hud::Frame path_frame{};
    path_frame.screen = frame.screen;
    path_frame.trajectory = true;
    path_frame.world_live = true;
    path_frame.eye = woke::game::Vec3d{0.0, 64.0, 0.0};
    path_frame.velocity = woke::game::Vec3d{0.0, 0.3, 1.2};
    path_frame.yaw_degrees = 0.0f;
    path_frame.pitch_degrees = 0.0f;
    path_frame.vertical_fov_degrees = 70.0f;
    path_frame.trajectory_style.params.seconds = 1.0;
    path_frame.trajectory_style.color = woke::modules::visual::Trajectories::color_for(2);
    WOKE_CHECK(hud::active(path_frame));

    std::array<visual::ScreenPoint, visual::kMaxTrajectoryPoints> projected{};
    const std::size_t visible = hud::project_trajectory(path_frame, projected);
    WOKE_CHECK(visible > 0);
    // The path starts ahead of the eye, so its first sample lands on screen near the centre.
    WOKE_CHECK(projected[0].x > 0.0f && projected[0].x < 1280.0f);

    // A trajectory with no live player view must not be drawn: a stale path is worse than none.
    path_frame.world_live = false;
    WOKE_CHECK(!hud::active(path_frame));
    {
        HeadlessFrame headless;
        hud::render(headless.list, path_frame);
        WOKE_CHECK(headless.vertices_added() == 0);
    }

    registry.reset();
}

} // namespace

void test_visual_modules() {
    test_trajectory_math();
    test_projection();
    test_crosshair_geometry();
    test_write_seam();
    test_write_modules();
    test_overlay_modules_and_hud();
}
