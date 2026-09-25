#include "test_harness.h"

// Step-8 module tests (roadmap step 8, sub-round 1: Movement + the action modules).
//
// The step's gate needs a game - "the Movement set and the action modules behave in singleplayer".
// What runs here is everything the game is not needed for, and each of these is a bug that would
// otherwise only surface in a live session:
//
//   * the bind-as-setting contract (a rebind is a persisted value, not session state),
//   * key dispatch: press-toggle, hold-to-activate, and an action module's fire-once key,
//   * Panic disabling everything through the registry, and
//   * the profile hotkeys' load/save alternation through the deferred-load mailbox,
//   * Auto Sprint holding the game's sprint key through the write seam, and
//   * the HUD velocity chip being drawn exactly when a live player view exists.
//
// What this cannot cover is the game's own reaction to a held sprint key - that is the manual
// in-game part of the gate (§12.2).

#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#include <imgui.h>

#include "core/config.h"
#include "modules/game_writes.h"
#include "modules/misc/config_hotkeys.h"
#include "modules/misc/panic.h"
#include "modules/module_manager.h"
#include "modules/movement/auto_sprint.h"
#include "modules/movement/velocity_display.h"
#include "ui/hud.h"
#include "ui/overlay.h"
#include "utils/render_utils.h"

namespace {

namespace movement = woke::modules::movement;
namespace misc = woke::modules::misc;
namespace draw = woke::ui::draw;
namespace hud = woke::ui::hud;

using woke::modules::Category;

[[nodiscard]] bool nearly(double a, double b, double epsilon = 0.001) noexcept {
    return (a > b ? a - b : b - a) <= epsilon;
}

// In-memory config backend, standing in for configs/<name>.json across a profile switch.
class MemoryStorage final : public woke::config::Storage {
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
        ++writes_;
        return true;
    }

    [[nodiscard]] bool exists(std::string_view path) const {
        return files_.find(std::string(path)) != files_.end();
    }

    std::map<std::string, std::string> files_;
    int writes_ = 0;
};

// Every fixture module in this file is stack-owned, so the process-wide registry must forget them
// before they die: a later test reading the registry (needs_render -> overlay::wanted()) would
// otherwise walk freed memory. Declared *before* the modules in each scope, so it is destroyed
// after them (locals die in reverse order) and the pointers are cleared last.
struct RegistryGuard {
    RegistryGuard() noexcept { woke::modules::manager().reset(); }
    ~RegistryGuard() noexcept { woke::modules::manager().reset(); }

    RegistryGuard(const RegistryGuard&) = delete;
    RegistryGuard& operator=(const RegistryGuard&) = delete;
};

// A headless frame handing out a real draw list, the same shape the widget and visual-module tests
// use: the HUD renderer only ever needs a draw list.
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
        ImGui::Begin("hud-chip-test", nullptr, ImGuiWindowFlags_NoDecoration);
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

// ── Fixture modules ──────────────────────────────────────────────────────────────

class ToggleProbe final : public woke::modules::BaseModule {
public:
    ToggleProbe() noexcept
        : BaseModule("Toggle Probe", "press toggles (dispatcher fixture)", Category::Misc, 0x50) {}

    bool on_enable() noexcept override {
        ++enables;
        return true;
    }

    void on_disable() noexcept override { ++disables; }

    int enables = 0;
    int disables = 0;
};

class HoldProbe final : public woke::modules::BaseModule {
public:
    HoldProbe() noexcept
        : BaseModule("Hold Probe", "held while down (dispatcher fixture)", Category::Movement,
              0x51) {}

    bool on_enable() noexcept override { return true; }
    void on_disable() noexcept override {}

    [[nodiscard]] bool hold_to_activate() const noexcept override { return true; }
};

class ActionProbe final : public woke::modules::BaseModule {
public:
    ActionProbe() noexcept
        : BaseModule("Action Probe", "fires once (dispatcher fixture)", Category::Misc, 0x52) {}

    bool on_enable() noexcept override { return false; }
    void on_disable() noexcept override {}

    [[nodiscard]] bool is_action() const noexcept override { return true; }

    [[nodiscard]] bool on_key(int virtual_key, bool down) noexcept override {
        if (!down || virtual_key != bind()) {
            return false;
        }
        ++fires;
        return true;
    }

    int fires = 0;
};

// ── Sections ─────────────────────────────────────────────────────────────────────

void test_key_dispatch() {
    woke_test::section("step 8 key dispatch");

    RegistryGuard guard;
    woke::modules::ModuleManager& registry = woke::modules::manager();

    ToggleProbe toggle;
    HoldProbe hold;
    ActionProbe action;
    WOKE_CHECK(registry.add(&toggle));
    WOKE_CHECK(registry.add(&hold));
    WOKE_CHECK(registry.add(&action));

    // Default hook: the press edge toggles, the release edge is ignored, and a second press flips
    // back. The enabled buckets follow without an explicit notify_changed().
    WOKE_CHECK(registry.handle_key(0x50, true));
    WOKE_CHECK(toggle.enabled());
    WOKE_CHECK(toggle.enables == 1);
    WOKE_CHECK(registry.category_enabled(woke::modules::Category::Misc) == 1);
    WOKE_CHECK(!registry.handle_key(0x50, false));
    WOKE_CHECK(toggle.enabled());
    WOKE_CHECK(registry.handle_key(0x50, true));
    WOKE_CHECK(!toggle.enabled());
    WOKE_CHECK(toggle.disables == 1);
    WOKE_CHECK(registry.category_enabled(woke::modules::Category::Misc) == 0);

    // Hold mode: the press enables, the release disables, and a repeated press is not a re-toggle.
    WOKE_CHECK(registry.handle_key(0x51, true));
    WOKE_CHECK(hold.enabled());
    WOKE_CHECK(registry.handle_key(0x51, false));
    WOKE_CHECK(!hold.enabled());
    WOKE_CHECK(!registry.handle_key(0x51, false)); // already released: nothing consumed

    // An action module claims its key and changes no state of its own.
    WOKE_CHECK(registry.handle_key(0x52, true));
    WOKE_CHECK(action.fires == 1);
    WOKE_CHECK(!action.enabled());
    WOKE_CHECK(!registry.handle_key(0x52, false));
    WOKE_CHECK(action.fires == 1);

    // Code 0 is "unbound" and is never routed.
    WOKE_CHECK(!registry.handle_key(0, true));

    // has_toggle_bind drives the toast layer: a toggle or hold module qualifies, an action module
    // does not (its key does something rather than flipping a state).
    WOKE_CHECK(registry.has_toggle_bind(0x50));
    WOKE_CHECK(registry.has_toggle_bind(0x51));
    WOKE_CHECK(!registry.has_toggle_bind(0x52));
    WOKE_CHECK(!registry.has_toggle_bind(0));
}

void test_bind_is_a_setting() {
    woke_test::section("step 8 bind as setting");

    RegistryGuard guard;
    ToggleProbe probe;
    WOKE_CHECK(woke::modules::manager().add(&probe));

    // Every module carries its bind as a setting, even one with no settings of its own.
    WOKE_CHECK(probe.setting_count() == 1);
    WOKE_CHECK_STR(probe.settings()[0]->name(), "bind");
    WOKE_CHECK(probe.settings()[0]->kind() == woke::settings::Kind::Bind);
    WOKE_CHECK(probe.settings()[0]->bind_value() == 0x50);

    // set_bind() is the setting write, so it marks dirty and the dispatcher sees the new key.
    probe.clear_dirty();
    probe.set_bind(0x60);
    WOKE_CHECK(probe.bind() == 0x60);
    WOKE_CHECK(probe.settings()[0]->dirty());
    WOKE_CHECK(probe.dirty());
    WOKE_CHECK(!woke::modules::manager().handle_key(0x50, true));
    WOKE_CHECK(woke::modules::manager().handle_key(0x60, true));
    WOKE_CHECK(probe.enabled());

    // A negative code is clamped to "unbound" rather than stored.
    probe.set_bind(-1);
    WOKE_CHECK(probe.bind() == 0);
    WOKE_CHECK(!woke::modules::manager().has_toggle_bind(0));
}

void test_panic() {
    woke_test::section("step 8 panic");

    RegistryGuard guard;
    woke::modules::ModuleManager& registry = woke::modules::manager();

    misc::Panic panic;
    ToggleProbe probe;
    movement::VelocityDisplay velocity;
    WOKE_CHECK(registry.add(&panic));
    WOKE_CHECK(registry.add(&probe));
    WOKE_CHECK(registry.add(&velocity));

    // It has no enabled state - and no settings of its own beyond the bind it persists.
    WOKE_CHECK(panic.is_action());
    WOKE_CHECK(!panic.enable());
    WOKE_CHECK(!panic.enabled());
    WOKE_CHECK(panic.setting_count() == 1);

    WOKE_CHECK(probe.enable());
    WOKE_CHECK(velocity.enable());
    registry.notify_changed();
    WOKE_CHECK(registry.enabled_count() == 2);

    panic.set_bind(0x53);
    WOKE_CHECK(registry.handle_key(0x53, true));
    WOKE_CHECK(registry.enabled_count() == 0);
    WOKE_CHECK(!probe.enabled());
    WOKE_CHECK(!velocity.enabled());
    WOKE_CHECK(registry.category_enabled(woke::modules::Category::Movement) == 0);

    // The key is claimed even with nothing live, so the game never also sees it - and the toast
    // layer is told there is nothing to announce, because Panic is an action module.
    WOKE_CHECK(registry.handle_key(0x53, true));
    WOKE_CHECK(!registry.has_toggle_bind(0x53));
    WOKE_CHECK(!registry.handle_key(0x53, false));
}

void test_config_hotkeys() {
    woke_test::section("step 8 config hotkeys");

    RegistryGuard guard;
    woke::modules::ModuleManager& registry = woke::modules::manager();

    misc::ConfigHotkeys hotkeys;
    ToggleProbe probe;
    WOKE_CHECK(registry.add(&hotkeys));
    WOKE_CHECK(registry.add(&probe));

    MemoryStorage storage;
    woke::config::set_storage(&storage);

    // Three binds of its own, plus the module's toggle bind from BaseModule.
    WOKE_CHECK(hotkeys.setting_count() == 4);
    WOKE_CHECK_STR(hotkeys.settings()[0]->name(), "profile1_key");
    WOKE_CHECK(hotkeys.slot_key(0) == 0); // unbound until the user arms it

    // A disabled hotkey set must not fire: the binds are a feature the user can turn off.
    hotkeys.settings()[0]->set_bind_value(0x60);
    WOKE_CHECK(!registry.handle_key(0x60, true));
    WOKE_CHECK(hotkeys.active_profile() == -1);

    WOKE_CHECK(hotkeys.enable());

    // First press: load. Nothing is on disk yet, so that is the documented first-boot case - the
    // request is still serviced, and the module becomes the active profile.
    WOKE_CHECK(registry.handle_key(0x60, true));
    WOKE_CHECK(hotkeys.active_profile() == 0);
    woke::config::LoadReport applied;
    WOKE_CHECK(woke::config::service_loads(applied));
    WOKE_CHECK(!applied.parsed); // no profile1.json yet: defaults stay, nothing is an error
    WOKE_CHECK(!woke::config::service_loads(applied)); // drained

    // Second press on the live profile: save the session's state over it.
    probe.enable();
    registry.notify_changed();
    WOKE_CHECK(registry.handle_key(0x60, true));
    WOKE_CHECK(hotkeys.active_profile() == -1);
    WOKE_CHECK(woke::config::service_saves());
    WOKE_CHECK(storage.exists(woke::config::path_for("profile1")));
    WOKE_CHECK(storage.files_[woke::config::path_for("profile1")].find("\"Toggle Probe\"")
        != std::string::npos);

    // Third press: load again, and this time the document exists and the state comes back.
    probe.disable();
    registry.notify_changed();
    WOKE_CHECK(registry.handle_key(0x60, true));
    WOKE_CHECK(woke::config::service_loads(applied));
    WOKE_CHECK(applied.parsed);
    WOKE_CHECK(applied.modules_matched == 2);
    WOKE_CHECK(probe.enabled());

    // Unbinding the slot stops it acting, and the profile keys are not toggle binds: the toast
    // layer has nothing to announce for them.
    hotkeys.settings()[0]->set_bind_value(0);
    WOKE_CHECK(!registry.handle_key(0x60, true));
    WOKE_CHECK(!registry.has_toggle_bind(0x60));
    WOKE_CHECK(hotkeys.setting_count() == 4);

    woke::config::set_storage(nullptr); // restore the real (file) store before the local dies
}

void test_velocity_display() {
    woke_test::section("step 8 velocity display");

    RegistryGuard guard;
    woke::modules::ModuleManager& registry = woke::modules::manager();

    movement::VelocityDisplay velocity;
    WOKE_CHECK(registry.add(&velocity));

    WOKE_CHECK(velocity.setting_count() == 2);
    WOKE_CHECK(velocity.units_index() == 0); // m/s out of the box

    // Horizontal speed only: a fall must not read as movement across the ground.
    WOKE_CHECK(nearly(movement::VelocityDisplay::horizontal_speed({3.0, -40.0, 4.0}), 5.0));
    WOKE_CHECK(nearly(movement::VelocityDisplay::horizontal_speed({0.0, 1.0, 0.0}), 0.0));

    WOKE_CHECK(nearly(movement::VelocityDisplay::to_display_unit(10.0, 0), 10.0));
    WOKE_CHECK(nearly(movement::VelocityDisplay::to_display_unit(10.0, 1), 36.0));
    WOKE_CHECK(nearly(movement::VelocityDisplay::to_display_unit(10.0, 2), 22.36936, 0.001));
    // An out-of-range index falls back to m/s rather than to zero.
    WOKE_CHECK(nearly(movement::VelocityDisplay::to_display_unit(10.0, 99), 10.0));

    // The module is an overlay module: enabling it keeps the frame pipeline alive (§7.8).
    WOKE_CHECK(!woke::ui::overlay::wanted());
    WOKE_CHECK(velocity.enable());
    WOKE_CHECK(woke::ui::overlay::wanted());
    WOKE_CHECK(velocity.disable());
    WOKE_CHECK(!woke::ui::overlay::wanted());
}

void test_velocity_chip() {
    woke_test::section("step 8 velocity chip");

    // The chip is drawn from the frame alone: no chip, no live world, nothing on screen.
    hud::Frame frame{};
    frame.screen = draw::Rect{0.0f, 0.0f, 1280.0f, 720.0f};
    frame.alpha = 1.0f;
    frame.velocity = woke::game::Vec3d{3.0, 0.0, 4.0};

    WOKE_CHECK(!hud::active(frame));
    frame.velocity_chip = true;
    WOKE_CHECK(!hud::active(frame)); // chip on, but no world read: nothing to show
    frame.world_live = true;
    WOKE_CHECK(hud::active(frame));

    {
        HeadlessFrame headless;
        hud::render(headless.list, frame);
        WOKE_CHECK(headless.vertices_added() > 0);
    }

    // A live world with the chip off draws nothing.
    hud::Frame idle{};
    idle.screen = frame.screen;
    idle.world_live = true;
    idle.velocity = frame.velocity;
    WOKE_CHECK(!hud::active(idle));
    {
        HeadlessFrame headless;
        hud::render(headless.list, idle);
        WOKE_CHECK(headless.vertices_added() == 0);
    }

    // The watermark still owns the first row; the chip takes the row beneath it.
    hud::Frame stacked{};
    stacked.screen = frame.screen;
    stacked.watermark = true;
    stacked.velocity_chip = true;
    stacked.world_live = true;
    stacked.frames_per_second = 60.0f;
    {
        HeadlessFrame headless;
        hud::render(headless.list, stacked);
        WOKE_CHECK(headless.vertices_added() > 0);
    }
}

void test_auto_sprint() {
    woke_test::section("step 8 auto sprint");

    RegistryGuard guard;

    movement::AutoSprint sprint;
    WOKE_CHECK(sprint.setting_count() == 2);
    WOKE_CHECK_STR(sprint.name(), "Auto Sprint");
    WOKE_CHECK(sprint.category() == woke::modules::Category::Movement);

    woke::modules::RecordingGameWrites recorder;
    recorder.seed(0.5f, 70.0f);
    recorder.seed_sprint(false); // the player is not holding sprint
    woke::modules::GameWrites* previous = woke::modules::set_game_writes(&recorder);

    // Enabling holds the key through the seam, and the baseline is captured for the restore.
    WOKE_CHECK(sprint.enable());
    WOKE_CHECK(recorder.sprint_active());
    WOKE_CHECK(recorder.sprint_pressed());
    WOKE_CHECK(!recorder.baseline_sprint()); // what the player's own key was doing
    WOKE_CHECK(recorder.sprint_applies() == 1);

    // The re-assert setting is on by default, so a tick re-holds the key: a focus change on any
    // other window clears the game's key state, and a one-shot write would quietly stop working.
    sprint.on_tick(0.05f);
    WOKE_CHECK(recorder.sprint_applies() == 2);

    sprint.settings()[0]->set_bool(false);
    sprint.on_tick(0.05f);
    WOKE_CHECK(recorder.sprint_applies() == 2); // one-shot: nothing to write

    // Disabling releases, restoring what the key was doing before.
    WOKE_CHECK(sprint.disable());
    WOKE_CHECK(!recorder.sprint_active());
    WOKE_CHECK(!recorder.sprint_pressed());
    const woke::game::Maybe<bool> released = woke::modules::game_writes().sprint_key_pressed();
    WOKE_CHECK(released.valid);
    WOKE_CHECK(!released.value);

    // A player who was already sprinting keeps sprinting after the toggle comes off.
    movement::AutoSprint sprint2;
    recorder.seed_sprint(true);
    WOKE_CHECK(sprint2.enable());
    WOKE_CHECK(sprint2.disable());
    WOKE_CHECK(recorder.sprint_pressed());

    // An unreachable binding (a stale mapping, no client instance) refuses the enable rather than
    // turning on and silently doing nothing.
    movement::AutoSprint sprint3;
    recorder.set_available(false);
    WOKE_CHECK(!sprint3.enable());
    WOKE_CHECK(!sprint3.enabled());

    recorder.set_available(true);
    WOKE_CHECK(woke::modules::set_game_writes(previous) == &recorder);
}

} // namespace

void test_step8_modules() {
    test_key_dispatch();
    test_bind_is_a_setting();
    test_panic();
    test_config_hotkeys();
    test_velocity_display();
    test_velocity_chip();
    test_auto_sprint();

    // The process-wide registry must be empty when this file returns: every module above was
    // stack-owned, and a later test reading the registry would walk freed memory otherwise.
    woke::modules::manager().reset();
}
