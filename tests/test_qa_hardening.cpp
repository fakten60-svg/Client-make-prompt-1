#include "test_harness.h"

// QA-hardening pass (v0.1.0 audit) - the gaps the existing suite did not cover.
//
// Every test here is portable: it drives pure helpers the Windows-only consumers call (the
// overlay policy predicate, the idempotency contract, the crosshair geometry), or the same
// registry/settings/config engine the DLL uses, through an in-memory storage backend. What
// still cannot be covered from a sandbox is the JNI reflection itself, MinHook against a real
// wglSwapBuffers, and an actual hot unload - those stay in docs/INGAME_QA.md.
//
// Sections:
//   * dt-independence across 30/60/144/240 Hz (the existing test pinned 60 vs 240),
//   * module enable/disable idempotency through the registry's own mutation path,
//   * a config round trip that touches every setting kind the engine can persist,
//   * all four crosshair shapes rendering non-empty geometry,
//   * the overlay suppression policy predicate in both directions,
//   * the deferred-save mailbox's coalescing and name-isolation contract (QA F1), and
//   * the portable mirror of the teardown audit: every bus channel reads empty after reset.

#include <array>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/config.h"
#include "core/event_bus.h"
#include "jni/mappings.h"
#include "core/events.h"
#include "modules/examples.h"
#include "modules/module_manager.h"
#include "modules/movement/velocity_display.h"
#include "modules/visual/custom_crosshair.h"
#include "modules/visual/hud_module.h"
#include "modules/visual/zoom.h"
#include "ui/animation/animation_controller.h"
#include "ui/hud.h"
#include "ui/overlay.h"
#include "utils/render_utils.h"

namespace {

namespace draw = woke::ui::draw;
namespace hud = woke::ui::hud;

using woke::ui::animation::AnimationController;

[[nodiscard]] bool nearly(float a, float b, float epsilon = 0.002f) noexcept {
    return (a > b ? a - b : b - a) <= epsilon;
}

// In-memory config backend that records which paths were written, so the mailbox tests can
// assert the *name* a serviced request carried and not only that a write happened.
class RecordingStorage final : public woke::config::Storage {
public:
    bool read(std::string_view path, std::string& out) override {
        const auto it = files_.find(std::string(path));
        if (it == files_.end()) {
            return false;
        }
        out = it->second;
        return true;
    }

    bool write(std::string_view path, std::string_view contents) override {
        files_[std::string(path)] = std::string(contents);
        written_paths_.emplace_back(path);
        return true;
    }

    [[nodiscard]] const std::vector<std::string>& written_paths() const noexcept {
        return written_paths_;
    }

    void clear_written() noexcept { written_paths_.clear(); }

private:
    std::vector<std::string> written_paths_;
    std::map<std::string, std::string> files_;
};

// A headless ImGui frame with a real draw list (same pattern as the other suites).
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
        ImGui::Begin("qa-test", nullptr, ImGuiWindowFlags_NoDecoration);
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

// Idempotency probe: counts how often the lifecycle hooks actually ran.
class Probe final : public woke::modules::BaseModule {
public:
    Probe() noexcept
        : BaseModule("QA Probe", "idempotency probe", woke::modules::Category::Misc, 0) {}

    bool on_enable() noexcept override {
        ++enables;
        return true;
    }
    void on_disable() noexcept override { ++disables; }

    int enables = 0;
    int disables = 0;
};

// One no-op handler per event channel, for the teardown-residue mirror below.
void qa_on_frame(woke::events::FrameEvent&) noexcept {}
void qa_on_tick(woke::events::TickEvent&) noexcept {}
void qa_on_key(woke::events::KeyEvent&) noexcept {}
void qa_on_mouse(woke::events::MouseEvent&) noexcept {}
void qa_on_focus(woke::events::FocusEvent&) noexcept {}
void qa_on_world(woke::events::WorldChangeEvent&) noexcept {}
void qa_on_player(woke::events::PlayerChangeEvent&) noexcept {}
void qa_on_config(woke::events::ConfigLoadedEvent&) noexcept {}
void qa_on_shutdown(woke::events::ShutdownEvent&) noexcept {}

} // namespace

void test_qa_hardening() {
    // ── dt-independence at the refresh rates players actually run ───────────────────
    woke_test::section("qa: dt-independence sweep (30/60/144/240 Hz)");
    {
        // The existing suite pins 1/60 vs 4x1/240; this sweeps the equal-total-time partition
        // across the refresh rates players actually run. Same elapsed time must land on the
        // same value regardless of how it is sliced.
        struct Rate {
            float dt;
            int steps;
        };
        constexpr Rate rates[] = {
            {1.0f / 30.0f, 15}, {1.0f / 60.0f, 30}, {1.0f / 144.0f, 72}, {1.0f / 240.0f, 120}};

        float reference_value = 0.0f;
        bool have_reference = false;
        for (const Rate& rate : rates) {
            AnimationController candidate;
            const auto handle = candidate.acquire_state(0.0f);
            candidate.set_target(handle, 1.0f);
            for (int step = 0; step < rate.steps; ++step) {
                candidate.tick(rate.dt);
            }
            const float value = candidate.value(handle);
            if (!have_reference) {
                reference_value = value;
                have_reference = true;
            } else {
                WOKE_CHECK(nearly(value, reference_value, 0.004f));
            }
        }

        // Full convergence at every cadence: after one second of ticks the state is at target.
        AnimationController done;
        const auto done_handle = done.acquire_state(0.0f);
        done.set_target(done_handle, 1.0f);
        for (int step = 0; step < 30; ++step) {
            done.tick(1.0f / 30.0f);
        }
        WOKE_CHECK(nearly(done.value(done_handle), 1.0f, 0.01f));
    }

    // ── Module enable/disable idempotency ───────────────────────────────────────────
    woke_test::section("qa: module lifecycle idempotency");
    {
        woke::modules::ModuleManager& registry = woke::modules::manager();
        registry.reset();
        Probe probe;
        woke::modules::visual::Zoom zoom;
        WOKE_CHECK(registry.add(&probe));
        WOKE_CHECK(registry.add(&zoom));

        WOKE_CHECK(probe.enable());
        WOKE_CHECK(!probe.enable());          // enabling twice is a no-op, not a second hook run
        WOKE_CHECK(probe.enables == 1);
        WOKE_CHECK(probe.disables == 0);
        WOKE_CHECK(probe.disable());
        WOKE_CHECK(!probe.disable());         // a no-op disable reports "nothing changed"
        WOKE_CHECK(probe.disables == 1);
        WOKE_CHECK(probe.enables == 1);

        WOKE_CHECK(probe.set_enabled(true));
        WOKE_CHECK(!probe.set_enabled(true)); // idempotent: no second enable fires
        WOKE_CHECK(probe.enables == 2);
        WOKE_CHECK(probe.set_enabled(false));
        WOKE_CHECK(!probe.set_enabled(false));
        WOKE_CHECK(probe.disables == 2);

        // The Zoom module refuses to enable when the write seam has no game behind it - the
        // honest "no game to affect" refusal - and the refusal leaves it off with no hook run.
        woke::modules::RecordingGameWrites unavailable;
        unavailable.set_available(false);
        woke::modules::GameWrites* const previous_seam =
            woke::modules::set_game_writes(&unavailable);
        WOKE_CHECK(!zoom.enable());
        WOKE_CHECK(!zoom.enabled());
        (void)woke::modules::set_game_writes(previous_seam);

        registry.reset();
    }

    // ── Config round trip across every persisted kind ───────────────────────────────
    woke_test::section("qa: config round trip (bool, slider, enum, bind, enabled)");
    {
        RecordingStorage storage;
        woke::config::set_storage(&storage); // returns void: nullptr restores the file store

        woke::modules::ModuleManager& registry = woke::modules::manager();
        registry.reset();
        woke::modules::visual::Zoom zoom;
        woke::modules::visual::CustomCrosshair crosshair;
        WOKE_CHECK(registry.add(&zoom));
        WOKE_CHECK(registry.add(&crosshair));

        woke::modules::RecordingGameWrites recorder;
        recorder.set_available(true);
        recorder.seed(0.5f, 70.0f); // the live baseline the module captures on enable
        woke::modules::GameWrites* const previous_seam =
            woke::modules::set_game_writes(&recorder);

        zoom.settings()[0]->set_float(5.5f);             // slider ("factor")
        zoom.settings()[1]->set_float(0.6f);             // slider ("smoothness")
        zoom.set_bind(0x51);                             // bind ("bind" = 'Q')
        crosshair.settings()[0]->set_enum_index(1);      // enum ("shape" = dot)
        crosshair.settings()[4]->set_enum_index(3);      // enum ("color" = red)
        WOKE_CHECK(zoom.enable());                       // the recorder backs the write seam here
        WOKE_CHECK(zoom.enabled());                      // bool ("enabled")
        WOKE_CHECK(crosshair.enable());

        WOKE_CHECK(woke::config::save("qa-roundtrip"));

        // Simulate a reinjection: fresh module objects, defaults restored.
        woke::modules::visual::Zoom reloaded;
        woke::modules::visual::CustomCrosshair reloaded_crosshair;
        registry.reset();
        WOKE_CHECK(registry.add(&reloaded));
        WOKE_CHECK(registry.add(&reloaded_crosshair));
        WOKE_CHECK(!reloaded.enabled());
        WOKE_CHECK(nearly(reloaded.settings()[0]->float_value(), 3.0f)); // Zoom's default factor

        const woke::config::LoadReport report = woke::config::load("qa-roundtrip");
        WOKE_CHECK(report.parsed);
        WOKE_CHECK(report.modules_matched == 2);
        WOKE_CHECK(report.wrong_type == 0);
        WOKE_CHECK(reloaded.enabled());
        WOKE_CHECK(reloaded_crosshair.enabled());
        WOKE_CHECK(nearly(reloaded.settings()[0]->float_value(), 5.5f));
        WOKE_CHECK(nearly(reloaded.settings()[1]->float_value(), 0.6f));
        WOKE_CHECK(reloaded.bind() == 0x51);
        WOKE_CHECK(reloaded_crosshair.settings()[0]->enum_index() == 1);
        WOKE_CHECK(reloaded_crosshair.settings()[4]->enum_index() == 3);

        // Unknown keys and wrong types are tolerated, counted, never fatal.
        const woke::config::LoadReport noisy =
            woke::config::deserialize("{\"Zoom\":{\"enabled\":true,\"nope\":1,\"factor\":\"x\"}}");
        WOKE_CHECK(noisy.parsed);
        WOKE_CHECK(noisy.unknown_settings == 1);
        WOKE_CHECK(noisy.wrong_type == 1);
        const woke::config::LoadReport garbage = woke::config::deserialize("not json at all");
        WOKE_CHECK(!garbage.parsed);

        (void)woke::modules::set_game_writes(previous_seam);
        woke::config::set_storage(nullptr);
        registry.reset();
    }

    // ── All crosshair shapes render non-empty geometry ──────────────────────────────
    woke_test::section("qa: crosshair geometry renders for every shape");
    {
        const woke::modules::visual::CrosshairShape shapes[] = {
            woke::modules::visual::CrosshairShape::Cross,
            woke::modules::visual::CrosshairShape::Dot,
            woke::modules::visual::CrosshairShape::Circle,
            woke::modules::visual::CrosshairShape::CrossDot,
        };
        for (const woke::modules::visual::CrosshairShape shape : shapes) {
            hud::Frame frame{};
            frame.screen = draw::Rect{0.0f, 0.0f, 1280.0f, 720.0f};
            frame.crosshair = true;
            frame.crosshair_style.shape = shape;
            frame.crosshair_style.color =
                woke::modules::visual::CustomCrosshair::color_for(0);
            frame.crosshair_style.size = 8.0f;
            frame.crosshair_style.gap = 3.0f;
            frame.crosshair_style.thickness = 1.5f;
            WOKE_CHECK(hud::active(frame));
            HeadlessFrame headless;
            hud::render(headless.list, frame);
            WOKE_CHECK(headless.vertices_added() > 0);
        }
    }

    // ── Overlay suppression policy, both directions ─────────────────────────────────
    woke_test::section("qa: overlay suppression predicate");
    {
        woke::modules::ModuleManager& registry = woke::modules::manager();
        registry.reset();
        woke::modules::visual::HudModule hud_module;
        woke::modules::movement::VelocityDisplay velocity;
        woke::modules::SprintState sprint;
        WOKE_CHECK(registry.add(&hud_module));
        WOKE_CHECK(registry.add(&velocity));
        WOKE_CHECK(registry.add(&sprint));

        // Nothing visible: zero ImGui work is wanted.
        WOKE_CHECK(!woke::ui::overlay::wanted());

        // The HUD's watermark is the "always-on chrome" case (the module must be enabled for
        // its outputs to count; a setting flipped on a disabled module draws nothing).
        WOKE_CHECK(hud_module.enable());
        WOKE_CHECK(woke::ui::overlay::wanted());
        hud_module.settings()[0]->set_bool(false); // watermark off, arraylist still on
        WOKE_CHECK(woke::ui::overlay::wanted());
        hud_module.settings()[1]->set_bool(false); // both outputs off: enabled but silent
        WOKE_CHECK(!woke::ui::overlay::wanted());

        // A single readout module keeps the frame pipeline alive.
        WOKE_CHECK(velocity.enable());
        WOKE_CHECK(woke::ui::overlay::wanted());
        WOKE_CHECK(velocity.disable());
        WOKE_CHECK(!woke::ui::overlay::wanted());

        // A module with no HUD output at all must not wake the renderer.
        WOKE_CHECK(sprint.enable());
        WOKE_CHECK(!woke::ui::overlay::wanted());
        WOKE_CHECK(sprint.disable());

        registry.reset();
    }

    // ── Deferred-save mailbox: coalescing and name isolation (QA F1) ────────────────
    woke_test::section("qa: config mailbox coalescing + name isolation");
    {
        RecordingStorage storage;
        woke::config::set_storage(&storage); // returns void: nullptr restores the file store

        woke::modules::ModuleManager& registry = woke::modules::manager();
        registry.reset();
        woke::modules::SprintState sprint;
        WOKE_CHECK(registry.add(&sprint));

        // Two requests before a service call collapse into one write.
        woke::config::request_save("qa-a");
        woke::config::request_save("qa-a");
        WOKE_CHECK(woke::config::service_saves());
        WOKE_CHECK(!woke::config::service_saves());
        WOKE_CHECK(storage.written_paths().size() == 1);
        WOKE_CHECK_STR(storage.written_paths()[0].c_str(), "configs/qa-a.json");

        // The name a serviced request carries belongs to the flag it consumed: the last poster
        // wins, the file is exactly theirs, and no torn mix of two names can be written.
        storage.clear_written();
        woke::config::request_save("qa-first");
        woke::config::request_save("qa-second");
        WOKE_CHECK(woke::config::service_saves());
        WOKE_CHECK(!woke::config::service_saves());
        WOKE_CHECK(storage.written_paths().size() == 1);
        WOKE_CHECK_STR(storage.written_paths()[0].c_str(), "configs/qa-second.json");

        woke::config::set_storage(nullptr);
        registry.reset();
    }

    // ── File store: the write lands through a temp file and leaves nothing behind (QA F2) ──
    woke_test::section("qa: file store temp-file write");
    {
        woke::config::set_storage(nullptr); // the default, file-backed store
        woke::modules::ModuleManager& registry = woke::modules::manager();
        registry.reset();
        woke::modules::SprintState sprint;
        WOKE_CHECK(registry.add(&sprint));
        WOKE_CHECK(sprint.enable());

        const std::string target = woke::config::path_for("qa-fileio");
        const std::string temp = target + ".tmp";
        WOKE_CHECK(woke::config::save("qa-fileio"));

        // The document is readable again through the same store (the rename replaced the target).
        const woke::config::LoadReport report = woke::config::load("qa-fileio");
        WOKE_CHECK(report.parsed);
        WOKE_CHECK(report.modules_matched == 1);

        // The sibling temp file the write went through is consumed, not left on disk.
        // (std::ifstream, not std::fopen: MSVC deprecates plain fopen — C4996.)
        std::ifstream leftover(temp, std::ios::binary);
        WOKE_CHECK(!leftover.is_open());

        (void)std::remove(target.c_str());
        registry.reset();
    }

    // ── Mappings version mismatch is a refusal (QA F3) ─────────────────────────
    woke_test::section("qa: mappings version gate");
    {
        // Everything else identical to the canonical schema; only "version" differs.
        constexpr const char* kMismatched = R"({
 "version": "1.20.4",
 "namespace": "intermediary",
 "classes": {
  "MinecraftClient": {
   "yarn": "net/minecraft/client/MinecraftClient",
   "intermediary": "net/minecraft/class_310",
   "aliases": ["MinecraftClient", "class_310", "net/minecraft/class_310"],
   "methods": {"getInstance": {"intermediary": "method_1551", "descriptor": "()Lnet/minecraft/class_310;"}},
   "fields": {}
  }
 }
})";
        woke::jni::Mappings mismatched;
        WOKE_CHECK(!mismatched.parse(kMismatched));
        WOKE_CHECK(!mismatched.loaded());
        WOKE_CHECK(!mismatched.warnings().empty());
        WOKE_CHECK_STR(mismatched.version().c_str(), "1.20.4");

        // An asset without a version field (schema variant predating it) stays accepted.
        constexpr const char* kUnversioned = R"({
 "MinecraftClient": {
  "yarn": "net/minecraft/client/MinecraftClient",
  "intermediary": "net/minecraft/class_310",
  "methods": {"getInstance": {"intermediary": "method_1551", "descriptor": "()Lnet/minecraft/class_310;"}}
 }
})";
        woke::jni::Mappings unversioned;
        WOKE_CHECK(unversioned.parse(kUnversioned));
        WOKE_CHECK(unversioned.loaded());
    }

    // ── Teardown residue mirror (portable half of the shutdown audit) ───────────────
    woke_test::section("qa: teardown audit - every bus channel reads empty after reset");
    {
        woke::events::Bus& bus = woke::events::bus();

        // Seed every channel with one subscriber, so the post-reset "0" is a read of a used
        // bus rather than a vacuous one. These mirror the nine event types shutdown() audits.
        const auto s1 = bus.subscribe<woke::events::FrameEvent, &qa_on_frame>();
        const auto s2 = bus.subscribe<woke::events::TickEvent, &qa_on_tick>();
        const auto s3 = bus.subscribe<woke::events::KeyEvent, &qa_on_key>();
        const auto s4 = bus.subscribe<woke::events::MouseEvent, &qa_on_mouse>();
        const auto s5 = bus.subscribe<woke::events::FocusEvent, &qa_on_focus>();
        const auto s6 = bus.subscribe<woke::events::WorldChangeEvent, &qa_on_world>();
        const auto s7 = bus.subscribe<woke::events::PlayerChangeEvent, &qa_on_player>();
        const auto s8 = bus.subscribe<woke::events::ConfigLoadedEvent, &qa_on_config>();
        const auto s9 = bus.subscribe<woke::events::ShutdownEvent, &qa_on_shutdown>();
        (void)s1; (void)s2; (void)s3; (void)s4; (void)s5;
        (void)s6; (void)s7; (void)s8; (void)s9;

        WOKE_CHECK(bus.handler_count<woke::events::FrameEvent>() == 1);
        WOKE_CHECK(bus.handler_count<woke::events::TickEvent>() == 1);
        WOKE_CHECK(bus.handler_count<woke::events::KeyEvent>() == 1);
        WOKE_CHECK(bus.handler_count<woke::events::MouseEvent>() == 1);
        WOKE_CHECK(bus.handler_count<woke::events::FocusEvent>() == 1);
        WOKE_CHECK(bus.handler_count<woke::events::WorldChangeEvent>() == 1);
        WOKE_CHECK(bus.handler_count<woke::events::PlayerChangeEvent>() == 1);
        WOKE_CHECK(bus.handler_count<woke::events::ConfigLoadedEvent>() == 1);
        WOKE_CHECK(bus.handler_count<woke::events::ShutdownEvent>() == 1);

        // reset() is what shutdown() relies on for "no subscriber survives an unload".
        bus.reset();
        WOKE_CHECK(bus.handler_count<woke::events::FrameEvent>() == 0);
        WOKE_CHECK(bus.handler_count<woke::events::TickEvent>() == 0);
        WOKE_CHECK(bus.handler_count<woke::events::KeyEvent>() == 0);
        WOKE_CHECK(bus.handler_count<woke::events::MouseEvent>() == 0);
        WOKE_CHECK(bus.handler_count<woke::events::FocusEvent>() == 0);
        WOKE_CHECK(bus.handler_count<woke::events::WorldChangeEvent>() == 0);
        WOKE_CHECK(bus.handler_count<woke::events::PlayerChangeEvent>() == 0);
        WOKE_CHECK(bus.handler_count<woke::events::ConfigLoadedEvent>() == 0);
        WOKE_CHECK(bus.handler_count<woke::events::ShutdownEvent>() == 0);
    }
}
