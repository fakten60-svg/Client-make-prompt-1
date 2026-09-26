#include "test_harness.h"

// Step-8 sub-round 2 tests (roadmap step 8: Combat + Mace).
//
// The step's gate needs a game - "the Combat and Mace readouts behave in singleplayer". What runs
// here is everything the game is not needed for, and each of these is a bug that would otherwise
// only surface in a live session:
//
//   * the smash-damage model (vanilla's fall-bonus curve, the smash threshold, the enhanced branch),
//   * every Combat/Mace module's settings being ordinary persisted values (so the Keybinds and
//     settings pages land with no per-module code),
//   * the session counters' stated rules - a hit extends a swing, an unavailable mace read keeps the
//     counters at zero, and the reset bind is the module's own setting,
//   * the overlay policy waking the frame pipeline for an enabled Combat/Mace readout, and
//   * the HUD renderer drawing each readout (and only it) from the frame the fill produces.
//
// What this cannot cover is the live JNI reads behind those frames - that is the manual in-game part
// of the gate (§12.2).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <imgui.h>

#include "modules/combat/attack_cooldown.h"
#include "modules/combat/combat_stats.h"
#include "modules/combat/reach_display.h"
#include "modules/combat/target_hud.h"
#include "modules/mace/mace_stats.h"
#include "modules/mace/smash_damage.h"
#include "modules/mace/smash_flash.h"
#include "modules/mace/smash_potential.h"
#include "modules/module_manager.h"
#include "ui/hud.h"
#include "ui/overlay.h"
#include "utils/render_utils.h"

namespace {

namespace combat = woke::modules::combat;
namespace mace = woke::modules::mace;
namespace hud = woke::ui::hud;
namespace draw = woke::ui::draw;

using woke::modules::Category;

[[nodiscard]] bool nearly(float a, float b, float epsilon = 0.001f) noexcept {
    return (a > b ? a - b : b - a) <= epsilon;
}

// Every fixture module here is stack-owned, so the process-wide registry must forget them before
// they die: overlay::wanted() and the HUD both walk the registry, and a stale pointer would be a
// use-after-free. Declared before the modules in each scope so it is destroyed after them.
struct RegistryGuard {
    RegistryGuard() noexcept { woke::modules::manager().reset(); }
    ~RegistryGuard() noexcept { woke::modules::manager().reset(); }

    RegistryGuard(const RegistryGuard&) = delete;
    RegistryGuard& operator=(const RegistryGuard&) = delete;
};

// A headless frame handing out a real draw list, the same shape the widget and step-8 tests use:
// the HUD renderer only ever needs a draw list.
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
        ImGui::Begin("combat-mace-test", nullptr, ImGuiWindowFlags_NoDecoration);
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

[[nodiscard]] hud::Frame screen_frame() noexcept {
    hud::Frame frame{};
    frame.screen = draw::Rect{0.0f, 0.0f, 1280.0f, 720.0f};
    frame.frames_per_second = 60.0f;
    return frame;
}

// ── Sections ─────────────────────────────────────────────────────────────────────

void test_smash_damage_model() {
    woke_test::section("step 8b smash damage model");

    // No fall: the mace's own base damage and no smash.
    const mace::SmashDamage grounded = mace::project_smash(0.0, false);
    WOKE_CHECK(nearly(grounded.total, mace::kMaceBaseDamage));
    WOKE_CHECK(!grounded.smash_ready);
    WOKE_CHECK(grounded.curve_steps == 0);

    // The first three blocks pay 4 damage each.
    const mace::SmashDamage one_step = mace::project_smash(3.0, false);
    WOKE_CHECK(one_step.curve_steps == 1);
    WOKE_CHECK(nearly(one_step.total, 10.0f)); // 6 base + 4
    WOKE_CHECK(one_step.smash_ready);

    const mace::SmashDamage three_steps = mace::project_smash(9.0, false);
    WOKE_CHECK(three_steps.curve_steps == 3);
    WOKE_CHECK(nearly(three_steps.total, 18.0f)); // 6 + 12

    // Beyond the third step the plain curve pays 1 per step, the enhanced curve pays 2.
    const mace::SmashDamage plain_five = mace::project_smash(15.0, false);
    WOKE_CHECK(plain_five.curve_steps == 5);
    WOKE_CHECK(nearly(plain_five.total, 20.0f)); // 6 + 12 + 2

    const mace::SmashDamage enhanced_five = mace::project_smash(15.0, true);
    WOKE_CHECK(nearly(enhanced_five.total, 22.0f)); // 6 + 12 + 4

    // The smash threshold is a hard edge, not a rounding accident.
    WOKE_CHECK(!mace::project_smash(1.4, false).smash_ready);
    WOKE_CHECK(mace::project_smash(1.5, false).smash_ready);

    // A nonsense (negative) fall is the grounded case, never a negative bonus.
    WOKE_CHECK(nearly(mace::project_smash(-4.0, false).total, mace::kMaceBaseDamage));

    // The damage bands the chip colours by.
    WOKE_CHECK(combat::AttackCooldown::kStyleCount == 2);
    WOKE_CHECK(mace::SmashPotential::band_for(11.9f) == 0);
    WOKE_CHECK(mace::SmashPotential::band_for(mace::SmashPotential::kNotableDamage) == 1);
    WOKE_CHECK(mace::SmashPotential::band_for(19.9f) == 1);
    WOKE_CHECK(mace::SmashPotential::band_for(mace::SmashPotential::kLethalDamage) == 2);
}

void test_module_settings() {
    woke_test::section("step 8b module settings");

    RegistryGuard guard;

    combat::TargetHud target_hud;
    WOKE_CHECK_STR(target_hud.name(), "Target HUD");
    WOKE_CHECK(target_hud.category() == Category::Combat);
    WOKE_CHECK(target_hud.setting_count() == 3); // position, scale, bind
    WOKE_CHECK_STR(target_hud.settings()[0]->enum_label().data(), "crosshair");
    WOKE_CHECK_STR(target_hud.settings()[0]->enum_label(1).data(), "corner");
    WOKE_CHECK(nearly(target_hud.scale(), 1.0f));
    WOKE_CHECK(nearly(target_hud.settings()[1]->float_minimum(), 0.75f));
    WOKE_CHECK(nearly(target_hud.settings()[1]->float_maximum(), 1.5f));
    target_hud.settings()[0]->set_enum_index(1);
    WOKE_CHECK(target_hud.position_index() == 1);

    combat::AttackCooldown cooldown;
    WOKE_CHECK_STR(cooldown.name(), "Attack Cooldown");
    WOKE_CHECK(cooldown.setting_count() == 3); // style, colour, bind
    WOKE_CHECK_STR(cooldown.settings()[0]->enum_label().data(), "bar");
    WOKE_CHECK_STR(cooldown.settings()[1]->enum_label(4).data(), "cyan");
    cooldown.settings()[1]->set_enum_index(4);
    WOKE_CHECK(cooldown.color_index() == 4);

    combat::ReachDisplay reach;
    WOKE_CHECK_STR(reach.name(), "Reach Display");
    WOKE_CHECK(reach.setting_count() == 1); // just the bind: the value is the game's own

    combat::CombatStats stats;
    WOKE_CHECK_STR(stats.name(), "Combat Stats");
    WOKE_CHECK(stats.setting_count() == 2); // reset bind + toggle bind

    mace::SmashPotential smash;
    WOKE_CHECK_STR(smash.name(), "Smash Potential");
    WOKE_CHECK(smash.setting_count() == 2);
    WOKE_CHECK(!smash.enhanced()); // the plain curve by default
    smash.settings()[0]->set_bool(true);
    WOKE_CHECK(smash.enhanced());

    mace::SmashFlash flash;
    WOKE_CHECK_STR(flash.name(), "Smash Flash");
    WOKE_CHECK(nearly(flash.intensity(), 0.5f));
    WOKE_CHECK(flash.setting_count() == 3);

    mace::MaceStats mace_stats;
    WOKE_CHECK_STR(mace_stats.name(), "Mace Stats");
    WOKE_CHECK(mace_stats.setting_count() == 2);
}

void test_session_counters() {
    woke_test::section("step 8b session counters");

    RegistryGuard guard;

    // Combat Stats: a hit extends the swing it belongs to, and only a below-full-strength swing is
    // counted as wasted.
    combat::CombatStats combat_stats;
    WOKE_CHECK(combat_stats.swings() == 0 && combat_stats.hits() == 0 && combat_stats.wasted() == 0);
    combat_stats.record_swing(false);
    combat_stats.record_swing(true);
    combat_stats.record_hit();
    WOKE_CHECK(combat_stats.swings() == 2);
    WOKE_CHECK(combat_stats.hits() == 1);
    WOKE_CHECK(combat_stats.wasted() == 1);

    // The reset bind is unbound by default - it must not be a key someone presses by accident.
    WOKE_CHECK(combat_stats.bind() == 0);
    WOKE_CHECK(!combat_stats.on_key(0x70, true));
    WOKE_CHECK(combat_stats.settings()[0]->kind() == woke::settings::Kind::Bind);
    combat_stats.settings()[0]->set_bind_value(0x70); // the reset bind, not the toggle bind
    WOKE_CHECK(!combat_stats.on_key(0x70, false)); // release does not reset
    WOKE_CHECK(combat_stats.on_key(0x70, true));
    WOKE_CHECK(combat_stats.swings() == 0 && combat_stats.hits() == 0 && combat_stats.wasted() == 0);

    // Mace Stats: a non-mace swing leaves every counter at zero, which is the honest reading when
    // the mace key is unavailable - never a count of the wrong item.
    mace::MaceStats mace_stats;
    mace_stats.record_swing(false, true, true);
    WOKE_CHECK(mace_stats.swings() == 0 && mace_stats.hits() == 0 && mace_stats.smashes() == 0);

    mace_stats.record_swing(true, true, true);
    mace_stats.record_swing(true, false, false);
    WOKE_CHECK(mace_stats.swings() == 2);
    WOKE_CHECK(mace_stats.hits() == 1);
    WOKE_CHECK(mace_stats.smashes() == 1);

    mace_stats.settings()[0]->set_bind_value(0x71);
    WOKE_CHECK(mace_stats.on_key(0x71, true));
    WOKE_CHECK(mace_stats.swings() == 0 && mace_stats.hits() == 0 && mace_stats.smashes() == 0);
}

void test_overlay_policy() {
    woke_test::section("step 8b overlay policy");

    RegistryGuard guard;
    WOKE_CHECK(!woke::ui::overlay::wanted()); // an empty registry wants no frames

    combat::CombatStats stats;
    WOKE_CHECK(woke::modules::manager().add(&stats));
    WOKE_CHECK(!woke::ui::overlay::wanted()); // registered but disabled

    WOKE_CHECK(stats.enable());
    WOKE_CHECK(woke::ui::overlay::wanted()); // an enabled counter is visible output

    WOKE_CHECK(stats.disable());
    WOKE_CHECK(!woke::ui::overlay::wanted());

    // A Mace readout wakes the pipeline the same way.
    mace::SmashPotential smash;
    WOKE_CHECK(woke::modules::manager().add(&smash));
    WOKE_CHECK(smash.enable());
    WOKE_CHECK(woke::ui::overlay::wanted());
}

void test_hud_readouts() {
    woke_test::section("step 8b HUD readouts");

    // The colour table is index-matched to the modules' enums and falls back for a bad index.
    const woke::util::Rgba accent = hud::accent_color_for(0);
    WOKE_CHECK(hud::accent_color_for(99).r == accent.r);
    WOKE_CHECK(hud::accent_color_for(99).g == accent.g);
    WOKE_CHECK(hud::accent_color_for(2).g != accent.g); // green differs from accent blue

    // An empty frame is inactive and draws nothing.
    {
        const hud::Frame idle = screen_frame();
        WOKE_CHECK(!hud::active(idle));
        HeadlessFrame headless;
        hud::render(headless.list, idle);
        WOKE_CHECK(headless.vertices_added() == 0);
    }

    // Target HUD: the card draws for a valid target with a live world.
    {
        hud::Frame frame = screen_frame();
        frame.target_card = true;
        frame.world_live = true;
        frame.target.valid = true;
        frame.target.name.assign("Player");
        frame.target.health = 14.0f;
        frame.target.max_health = 20.0f;
        frame.target.distance = 2.5f;
        WOKE_CHECK(hud::active(frame));
        HeadlessFrame headless;
        hud::render(headless.list, frame);
        WOKE_CHECK(headless.vertices_added() > 0);
    }

    // The cooldown bar and the arc are both drawable.
    for (std::size_t style = 0; style < combat::AttackCooldown::kStyleCount; ++style) {
        hud::Frame frame = screen_frame();
        frame.cooldown_bar = true;
        frame.world_live = true;
        frame.cooldown_style = style;
        frame.cooldown_progress = 0.5f;
        HeadlessFrame headless;
        hud::render(headless.list, frame);
        WOKE_CHECK(headless.vertices_added() > 0);
    }

    // Reach, smash and the flash each render on their own.
    {
        hud::Frame frame = screen_frame();
        frame.reach_chip = true;
        frame.world_live = true;
        frame.reach_blocks = 3.0f;
        frame.smash_chip = true;
        frame.fall_distance = 12.0;
        frame.smash_flash = true;
        frame.flash_intensity = 0.8f;
        WOKE_CHECK(hud::active(frame));
        HeadlessFrame headless;
        hud::render(headless.list, frame);
        WOKE_CHECK(headless.vertices_added() > 0);
    }

    // The counter chips are session data: they draw with no live world at all.
    {
        hud::Frame frame = screen_frame();
        frame.combat_counters = true;
        frame.counter_swings = 12;
        frame.counter_hits = 5;
        frame.counter_wasted = 3;
        frame.mace_counters = true;
        frame.mace_swing_count = 4;
        frame.mace_hit_count = 2;
        frame.mace_smash_count = 1;
        WOKE_CHECK(hud::active(frame));
        HeadlessFrame headless;
        hud::render(headless.list, frame);
        WOKE_CHECK(headless.vertices_added() > 0);
    }

    // A combat readout with no live world must not draw the world-dependent chips.
    {
        hud::Frame frame = screen_frame();
        frame.target_card = true;
        frame.target.valid = true;
        frame.target.name.assign("Ghost");
        frame.target.distance = 4.0f;
        frame.world_live = false;
        HeadlessFrame headless;
        hud::render(headless.list, frame);
        WOKE_CHECK(headless.vertices_added() == 0);
    }
}

} // namespace

void test_combat_mace() {
    test_smash_damage_model();
    test_module_settings();
    test_session_counters();
    test_overlay_policy();
    test_hud_readouts();

    // The registry must be empty when this file returns: every fixture above was stack-owned.
    woke::modules::manager().reset();
}
