#include "test_harness.h"

// Step-8 sub-round 3 tests (roadmap step 8: Misc / Movement / Spear).
//
// The step's gate needs a game - "the Misc, Movement and Spear modules behave in singleplayer". What
// runs here is everything the game is not needed for, and each of these is a bug that would
// otherwise only surface in a live session:
//
//   * the friend list's duplicate/full/remove rules (a list that double-adds or truncates wrong is a
//     count the HUD lies about),
//   * Client Sound's per-cue gating and its refusal to fire while disabled or muted,
//   * Safe Walk holding the game's *own* sneak key through the write seam (and refusing to enable
//     when that binding is unreachable),
//   * the Loyalty HUD's throw/return state machine,
//   * the overlay policy waking the frame pipeline for a HUD-bearing module and *not* for Client
//     Sound, which has no HUD output, and
//   * the HUD renderer drawing each step-8c chip (and only it) from the frame the fill produces.
//
// What this cannot cover is the live JNI reads behind those frames - that is the manual in-game part
// of the gate (§12.2).

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <imgui.h>

#include "modules/game_writes.h"
#include "modules/misc/client_sound.h"
#include "modules/misc/friend_manager.h"
#include "modules/module_manager.h"
#include "modules/movement/safe_walk.h"
#include "modules/spear/loyalty_hud.h"
#include "modules/spear/riptide_indicator.h"
#include "modules/spear/trident_cooldown.h"
#include "ui/hud.h"
#include "ui/overlay.h"
#include "utils/render_utils.h"

namespace {

namespace misc = woke::modules::misc;
namespace movement = woke::modules::movement;
namespace spear = woke::modules::spear;
namespace hud = woke::ui::hud;
namespace draw = woke::ui::draw;

using woke::modules::Category;

[[nodiscard]] bool nearly(double a, double b, double epsilon = 0.001) noexcept {
    return (a > b ? a - b : b - a) <= epsilon;
}

// Every fixture module is stack-owned, so the process-wide registry must forget them before they
// die. Declared before the modules in each scope so it is destroyed after them.
struct RegistryGuard {
    RegistryGuard() noexcept { woke::modules::manager().reset(); }
    ~RegistryGuard() noexcept { woke::modules::manager().reset(); }

    RegistryGuard(const RegistryGuard&) = delete;
    RegistryGuard& operator=(const RegistryGuard&) = delete;
};

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
        ImGui::Begin("spear-misc-test", nullptr, ImGuiWindowFlags_NoDecoration);
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

void test_friend_manager() {
    woke_test::section("step 8c friend manager");

    misc::FriendManager friends;
    WOKE_CHECK_STR(friends.name(), "Friend Manager");
    WOKE_CHECK(friends.category() == Category::Misc);
    WOKE_CHECK(friends.empty());

    WOKE_CHECK(friends.add("Steve"));
    WOKE_CHECK(friends.add("Alex"));
    WOKE_CHECK(!friends.add("Steve")); // duplicate
    WOKE_CHECK(!friends.add(""));      // empty
    WOKE_CHECK(friends.count() == 2);
    WOKE_CHECK(friends.contains("Alex"));
    WOKE_CHECK(!friends.contains("Herobrine"));

    // Fill to capacity with unique names, then refuse one more.
    friends.clear();
    char name[16];
    for (std::size_t index = 0; index < misc::FriendManager::kMaxFriends; ++index) {
        std::snprintf(name, sizeof(name), "f%zu", index);
        WOKE_CHECK(friends.add(name));
    }
    WOKE_CHECK(friends.count() == misc::FriendManager::kMaxFriends);
    WOKE_CHECK(!friends.add("one_too_many"));

    WOKE_CHECK(friends.remove("f0"));
    WOKE_CHECK(!friends.contains("f0"));
    WOKE_CHECK(friends.add("one_more"));
    WOKE_CHECK(!friends.remove("Nobody"));

    // The colour is an ordinary enum setting; a bad index clamps through the setting's own guard.
    friends.settings()[0]->set_enum_index(4);
    WOKE_CHECK(friends.color_index() == 4);
    friends.settings()[0]->set_enum_index(99);
    WOKE_CHECK(friends.color_index() == 4); // rejected, unchanged

    friends.clear();
    WOKE_CHECK(friends.empty());
}

void test_client_sound() {
    woke_test::section("step 8c client sound");

    misc::RecordingClientSoundSink recorder;
    misc::ClientSoundSink* previous = misc::set_client_sound_sink(&recorder);

    misc::ClientSound sound;
    WOKE_CHECK_STR(sound.name(), "Client Sound");
    WOKE_CHECK(sound.setting_count() == 4); // toggles, success, volume, bind

    // Disabled: nothing plays, whatever the cue.
    WOKE_CHECK(!sound.play(misc::SoundCue::Toggle));
    WOKE_CHECK(recorder.plays() == 0);

    WOKE_CHECK(sound.enable());
    WOKE_CHECK(sound.play(misc::SoundCue::Toggle));
    WOKE_CHECK(recorder.plays() == 1);
    WOKE_CHECK(recorder.last_cue() == misc::SoundCue::Toggle);
    WOKE_CHECK(nearly(recorder.last_volume(), sound.volume()));

    // A cue whose toggle is off stays silent; a warning ignores the toggle preference.
    sound.settings()[0]->set_bool(false); // toggles off
    WOKE_CHECK(!sound.play(misc::SoundCue::Toggle));
    WOKE_CHECK(recorder.plays() == 1);
    WOKE_CHECK(sound.play(misc::SoundCue::Warning));
    WOKE_CHECK(recorder.plays() == 2);

    // Volume zero mutes everything, including warnings.
    sound.settings()[2]->set_float(0.0f);
    WOKE_CHECK(!sound.play(misc::SoundCue::Warning));
    WOKE_CHECK(recorder.plays() == 2);

    WOKE_CHECK(sound.disable());
    WOKE_CHECK(misc::set_client_sound_sink(previous) == &recorder);
}

void test_safe_walk() {
    woke_test::section("step 8c safe walk");

    RegistryGuard guard;

    movement::SafeWalk walk;
    WOKE_CHECK_STR(walk.name(), "Safe Walk");
    WOKE_CHECK(walk.category() == Category::Movement);
    WOKE_CHECK(walk.setting_count() == 2); // reassert, bind

    woke::modules::RecordingGameWrites recorder;
    recorder.seed(0.5f, 70.0f);
    recorder.seed_sneak(false); // the player is not holding sneak
    woke::modules::GameWrites* previous = woke::modules::set_game_writes(&recorder);

    WOKE_CHECK(walk.enable());
    WOKE_CHECK(recorder.sneak_active());
    WOKE_CHECK(recorder.sneak_pressed());
    WOKE_CHECK(!recorder.baseline_sneak());
    WOKE_CHECK(recorder.sneak_applies() == 1);

    // The re-assert setting is on by default, so a tick re-holds the key.
    walk.on_tick(0.05f);
    WOKE_CHECK(recorder.sneak_applies() == 2);

    WOKE_CHECK(walk.engaged());
    WOKE_CHECK(walk.disable());
    WOKE_CHECK(!recorder.sneak_active());
    WOKE_CHECK(!recorder.sneak_pressed());
    WOKE_CHECK(!walk.engaged());

    // A player who was already sneaking keeps sneaking after the toggle comes off.
    movement::SafeWalk walk2;
    recorder.seed_sneak(true);
    WOKE_CHECK(walk2.enable());
    WOKE_CHECK(walk2.disable());
    WOKE_CHECK(recorder.sneak_pressed());

    // An unreachable binding refuses the enable rather than pretending to help.
    movement::SafeWalk walk3;
    recorder.set_available(false);
    WOKE_CHECK(!walk3.enable());
    WOKE_CHECK(!walk3.enabled());

    recorder.set_available(true);
    WOKE_CHECK(woke::modules::set_game_writes(previous) == &recorder);
}

void test_spear_modules() {
    woke_test::section("step 8c spear modules");

    RegistryGuard guard;

    spear::RiptideIndicator riptide;
    WOKE_CHECK_STR(riptide.name(), "Riptide Indicator");
    WOKE_CHECK(riptide.category() == Category::Spear);
    WOKE_CHECK(riptide.requires_trident()); // the chip is a trident fact by default
    riptide.settings()[0]->set_bool(false);
    WOKE_CHECK(!riptide.requires_trident());

    spear::TridentCooldown trident;
    WOKE_CHECK_STR(trident.name(), "Trident Cooldown");
    WOKE_CHECK(trident.setting_count() == 2); // colour + bind
    WOKE_CHECK_STR(trident.settings()[0]->enum_label().data(), "cyan");
    trident.settings()[0]->set_enum_index(3);
    WOKE_CHECK(trident.color_index() == 3);

    spear::LoyaltyHud loyalty;
    WOKE_CHECK_STR(loyalty.name(), "Loyalty HUD");
    WOKE_CHECK(loyalty.setting_count() == 2); // reset bind + toggle bind
    WOKE_CHECK(!loyalty.tracking());
    WOKE_CHECK(nearly(loyalty.elapsed(), 0.0));
}

void test_loyalty_state_machine() {
    woke_test::section("step 8c loyalty state machine");

    spear::LoyaltyHud loyalty;

    // In hand: nothing is being tracked.
    loyalty.observe(0.05, true);
    WOKE_CHECK(!loyalty.tracking());
    WOKE_CHECK(nearly(loyalty.elapsed(), 0.0));

    // Out of hand: the trip starts and the timer accumulates the frame deltas it is given.
    loyalty.observe(0.5, false);
    loyalty.observe(0.5, false);
    WOKE_CHECK(loyalty.tracking());
    WOKE_CHECK(nearly(loyalty.elapsed(), 1.0));

    // Back in hand: the trip completes and the timer resets for the next one.
    loyalty.observe(0.5, true);
    WOKE_CHECK(!loyalty.tracking());
    WOKE_CHECK(nearly(loyalty.last_trip(), 1.0));
    WOKE_CHECK(nearly(loyalty.elapsed(), 0.0));

    // A negative/zero delta cannot move time backwards.
    loyalty.observe(-1.0, false);
    WOKE_CHECK(nearly(loyalty.elapsed(), 0.0));

    // The reset bind clears both the live timer and the last completed trip.
    loyalty.settings()[0]->set_bind_value(0x72);
    WOKE_CHECK(loyalty.on_key(0x72, true));
    WOKE_CHECK(!loyalty.tracking());
    WOKE_CHECK(nearly(loyalty.last_trip(), 0.0));
    WOKE_CHECK(!loyalty.on_key(0x72, false));
}

void test_overlay_policy() {
    woke_test::section("step 8c overlay policy");

    RegistryGuard guard;
    WOKE_CHECK(!woke::ui::overlay::wanted());

    misc::FriendManager friends;
    WOKE_CHECK(woke::modules::manager().add(&friends));
    WOKE_CHECK(friends.enable());
    WOKE_CHECK(woke::ui::overlay::wanted());
    WOKE_CHECK(friends.disable());
    WOKE_CHECK(!woke::ui::overlay::wanted());

    // Client Sound has no HUD output, so an enabled Client Sound must not keep the frame pipeline
    // alive on its own.
    misc::ClientSound sound;
    WOKE_CHECK(woke::modules::manager().add(&sound));
    WOKE_CHECK(sound.enable());
    WOKE_CHECK(!woke::ui::overlay::wanted());
}

void test_hud_readouts() {
    woke_test::section("step 8c HUD readouts");

    // An empty frame is inactive and draws nothing.
    {
        const hud::Frame idle = screen_frame();
        WOKE_CHECK(!hud::active(idle));
        HeadlessFrame headless;
        hud::render(headless.list, idle);
        WOKE_CHECK(headless.vertices_added() == 0);
    }

    // The local chips (friend count, safe-walk state) draw with no live world.
    {
        hud::Frame frame = screen_frame();
        frame.friend_chip = true;
        frame.friend_count = 3;
        frame.safe_walk_chip = true;
        frame.safe_walk_engaged = true;
        WOKE_CHECK(hud::active(frame));
        HeadlessFrame headless;
        hud::render(headless.list, frame);
        WOKE_CHECK(headless.vertices_added() > 0);
    }

    // The spear chips draw with a live world.
    {
        hud::Frame frame = screen_frame();
        frame.riptide_chip = true;
        frame.riptide_engaged = true;
        frame.riptide_trident = true;
        frame.trident_chip = true;
        frame.trident_progress = 0.62f;
        frame.loyalty_chip = true;
        frame.loyalty_tracking = true;
        frame.loyalty_seconds = 2.5;
        frame.world_live = true;
        WOKE_CHECK(hud::active(frame));
        HeadlessFrame headless;
        hud::render(headless.list, frame);
        WOKE_CHECK(headless.vertices_added() > 0);
    }

    // Without a live world the spear chips must not draw.
    {
        hud::Frame frame = screen_frame();
        frame.riptide_chip = true;
        frame.trident_chip = true;
        frame.loyalty_chip = true;
        frame.world_live = false;
        WOKE_CHECK(!hud::active(frame));
        HeadlessFrame headless;
        hud::render(headless.list, frame);
        WOKE_CHECK(headless.vertices_added() == 0);
    }
}

} // namespace

void test_spear_misc() {
    test_friend_manager();
    test_client_sound();
    test_safe_walk();
    test_spear_modules();
    test_loyalty_state_machine();
    test_overlay_policy();
    test_hud_readouts();

    // The registry must be empty when this file returns: every fixture above was stack-owned.
    woke::modules::manager().reset();
}
