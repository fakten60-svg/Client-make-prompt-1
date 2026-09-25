#include "test_harness.h"

// Widget-library tests (roadmap step 6).
//
// The step's gate is "the GUI's categories show real entries, and a toggle persists". The GUI
// composition itself is Windows-only (windows.h plus the game's GL context), so the widgets are
// exercised exactly the way the composition uses them: a headless ImGui context hands each
// component a real ImDrawList, the input each component consumes is the same struct the GUI fills
// in once per frame, and the last section drives the whole click -> module -> save -> reload path
// through the real registry and the injectable config backend.
//
// The three things that are actually likely to be wrong - the key-name table, the capture state
// machine and the toast pool's lifetime/stacking - are asserted directly, because each of them is
// a user-visible bug that a screenshot would not catch.
//
// What this cannot cover is how the widgets look; that is the in-game gate (§12.2, step 6).

#include <cstddef>
#include <map>
#include <string>

#include <imgui.h>

#include "core/config.h"
#include "modules/examples.h"
#include "modules/module_manager.h"
#include "ui/animation/animation_controller.h"
#include "ui/components/keybind_badge.h"
#include "ui/components/module_card.h"
#include "ui/components/pill_toggle.h"
#include "ui/components/search_bar.h"
#include "ui/components/sidebar.h"
#include "ui/notifications.h"
#include "ui/theme.h"
#include "utils/render_utils.h"

namespace {

namespace draw = woke::ui::draw;

using draw::Rect;
using woke::ui::animation::AnimationController;
using woke::ui::components::Input;
using woke::ui::components::KeybindBadge;
using woke::ui::components::ModuleCard;
using woke::ui::components::PillToggle;
using woke::ui::components::SearchBar;
using woke::ui::components::Sidebar;
using woke::ui::Notifications;
using woke::ui::Section;
using woke::ui::Toast;
using woke::ui::ToastIcon;

[[nodiscard]] bool nearly(float a, float b, float epsilon = 0.01f) noexcept {
    return (a > b ? a - b : b - a) <= epsilon;
}

// ── Input snapshots, matching how the GUI fills them from ImGuiIO ────────────────

void hover(Input& input, float x, float y) noexcept {
    input.mouse_x = x;
    input.mouse_y = y;
    input.down[0] = false;
    input.pressed[0] = false;
    input.released[0] = false;
}

void press(Input& input, float x, float y) noexcept {
    input.mouse_x = x;
    input.mouse_y = y;
    input.down[0] = true;
    input.pressed[0] = true;
    input.released[0] = false;
}

void release(Input& input, float x, float y) noexcept {
    input.mouse_x = x;
    input.mouse_y = y;
    input.down[0] = false;
    input.pressed[0] = false;
    input.released[0] = true;
}

// ── A headless frame with a real ImDrawList ──────────────────────────────────────
//
// Components only ever need a draw list, so no window layout is involved beyond the one ImGui
// insists on to hand one out. `RendererHasTextures` is what the OpenGL3 backend advertises too,
// and without it ImGui asserts that a backend uploaded the font atlas.
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
        ImGui::Begin("widget-test", nullptr, ImGuiWindowFlags_NoDecoration);
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

// Components only steer their animation targets; the GUI owns the single controller tick per
// frame (§7.4). The tests own it here.
template <typename Steer>
void run_frames(AnimationController& animation, int frames, Steer steer) {
    for (int frame = 0; frame < frames; ++frame) {
        steer();
        animation.tick(1.0f / 60.0f);
    }
}

// In-memory config backend, standing in for configs/default.json across a reinjection.
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
        return true;
    }

    [[nodiscard]] bool exists(std::string_view path) const {
        return files_.find(std::string(path)) != files_.end();
    }

    [[nodiscard]] const std::string& at(std::string_view path) const {
        return files_.at(std::string(path));
    }

    std::map<std::string, std::string> files_;
};

} // namespace

void test_widgets() {
    // ── Key names: a wrong label is a bind the user cannot trust ──────────────────
    woke_test::section("widget keybind badge");
    char label[16];

    woke::ui::components::format_key(0, label, sizeof(label));
    WOKE_CHECK_STR(label, "NONE");
    woke::ui::components::format_key('A', label, sizeof(label));
    WOKE_CHECK_STR(label, "A");
    woke::ui::components::format_key('7', label, sizeof(label));
    WOKE_CHECK_STR(label, "7");
    woke::ui::components::format_key(woke::ui::components::kKeyF1, label, sizeof(label));
    WOKE_CHECK_STR(label, "F1");
    woke::ui::components::format_key(woke::ui::components::kKeyF12, label, sizeof(label));
    WOKE_CHECK_STR(label, "F12");
    woke::ui::components::format_key(woke::ui::components::kKeyEscape, label, sizeof(label));
    WOKE_CHECK_STR(label, "ESC");
    woke::ui::components::format_key(woke::ui::components::kKeyRightShift, label, sizeof(label));
    WOKE_CHECK_STR(label, "RSHIFT");
    woke::ui::components::format_key(woke::ui::components::kKeySpace, label, sizeof(label));
    WOKE_CHECK_STR(label, "SPACE");
    woke::ui::components::format_key(woke::ui::components::kKeyEnter, label, sizeof(label));
    WOKE_CHECK_STR(label, "ENTER");
    // An unnamed code still gets an honest chip label rather than an empty one.
    woke::ui::components::format_key(0x0C, label, sizeof(label));
    WOKE_CHECK_STR(label, "0x0C");
    // A capacity that cannot hold the label must still terminate the buffer.
    char tiny[3];
    woke::ui::components::format_key(woke::ui::components::kKeyBackspace, tiny, sizeof(tiny));
    WOKE_CHECK(tiny[2] == '\0');
    WOKE_CHECK(tiny[0] != '\0');
    woke::ui::components::format_key('A', nullptr, 0); // a null buffer is a no-op, not a crash
    woke::ui::components::format_key('A', label, 0);

    // Capture state machine: arming, rebinding, cancelling. This is the part that decides whether
    // a click can bind the wrong key or leave the card stuck in capture mode.
    AnimationController animation;
    KeybindBadge badge;
    badge.bind(animation);
    badge.set_area(Rect{200.0f, 100.0f, 60.0f, 18.0f});
    badge.set_bind('C');
    WOKE_CHECK(badge.bind_key() == 'C');
    WOKE_CHECK(!badge.empty());
    WOKE_CHECK(!badge.capture_armed());

    // Unarmed, the badge consumes nothing: a key must never be stolen by a chip nobody clicked.
    WOKE_CHECK(!badge.accept_key('X'));
    WOKE_CHECK(badge.bind_key() == 'C');

    Input input;
    hover(input, 400.0f, 400.0f);
    WOKE_CHECK(!badge.handle_input(input)); // outside: not ours
    WOKE_CHECK(!badge.take_click());

    press(input, 230.0f, 109.0f);
    WOKE_CHECK(badge.handle_input(input)); // a press on the chip is consumed
    WOKE_CHECK(badge.take_click());
    WOKE_CHECK(!badge.take_click()); // one-shot: the release frame cannot act on it again

    badge.set_capture_armed(true);
    WOKE_CHECK(badge.capture_armed());
    WOKE_CHECK(badge.accept_key(woke::ui::components::kKeyEscape)); // ESC cancels
    WOKE_CHECK(!badge.capture_armed());
    WOKE_CHECK(badge.bind_key() == 'C'); // unchanged

    badge.set_capture_armed(true);
    WOKE_CHECK(badge.accept_key('G'));
    WOKE_CHECK(badge.bind_key() == 'G');
    WOKE_CHECK(!badge.capture_armed());
    // Code 0 cannot become a bind: "unbound" is reached by unbinding, not by capturing a no-key.
    badge.set_capture_armed(true);
    WOKE_CHECK(badge.accept_key(0));
    WOKE_CHECK(badge.bind_key() == 'G');

    // Hover and capture pulse animate from the same controller, so the ring cannot stay lit.
    badge.set_capture_armed(true);
    badge.set_area(Rect{200.0f, 100.0f, 60.0f, 18.0f});
    press(input, 230.0f, 109.0f);
    (void)badge.handle_input(input);
    run_frames(animation, 30, [&] { badge.animate(1.0f / 60.0f); });
    WOKE_CHECK(badge.hover_amount() > 0.9f);
    WOKE_CHECK(badge.pulse_amount() > 0.9f);
    badge.set_capture_armed(false);
    run_frames(animation, 30, [&] { badge.animate(1.0f / 60.0f); });
    WOKE_CHECK(badge.hover_amount() > 0.9f); // still hovered
    WOKE_CHECK(badge.pulse_amount() < 0.05f); // the ring went out

    {
        HeadlessFrame frame;
        badge.set_alpha(1.0f);
        badge.render(frame.list, Rect{200.0f, 100.0f, 60.0f, 18.0f});
        WOKE_CHECK(frame.vertices_added() > 0);
        badge.set_alpha(0.0f);
        const int hidden_before = frame.list->VtxBuffer.Size;
        badge.render(frame.list, Rect{200.0f, 100.0f, 60.0f, 18.0f});
        WOKE_CHECK(frame.list->VtxBuffer.Size == hidden_before); // fully transparent draws nothing
        badge.render(nullptr, Rect{200.0f, 100.0f, 60.0f, 18.0f});
        badge.render(frame.list, Rect{});
    }

    badge.unbind();
    WOKE_CHECK(animation.active_state_count() == 0); // hover + capture pulse both returned
    badge.unbind();                                  // idempotent

    // ── Pill toggle: one value drives the knob and the colour ─────────────────────
    woke_test::section("widget pill toggle");
    AnimationController pill_animation;
    PillToggle pill;
    pill.bind(pill_animation);
    pill.set_area(Rect{100.0f, 100.0f, 40.0f, 22.0f});
    WOKE_CHECK(!pill.value());
    WOKE_CHECK(nearly(pill.amount(), 0.0f));

    pill.set_value(true);
    run_frames(pill_animation, 40, [&] { pill.animate(1.0f / 60.0f); });
    WOKE_CHECK(pill.amount() > 0.98f);

    pill.set_value(false);
    run_frames(pill_animation, 40, [&] { pill.animate(1.0f / 60.0f); });
    WOKE_CHECK(pill.amount() < 0.02f);

    // Standalone, the pill owns its own click; the card overrides that by never handing it input.
    hover(input, 500.0f, 500.0f);
    WOKE_CHECK(!pill.handle_input(input));
    press(input, 120.0f, 111.0f);
    WOKE_CHECK(pill.handle_input(input));
    WOKE_CHECK(pill.take_click());
    WOKE_CHECK(pill.value()); // flipped on
    WOKE_CHECK(!pill.take_click());

    {
        HeadlessFrame frame;
        pill.set_alpha(1.0f);
        pill.render(frame.list, Rect{100.0f, 100.0f, 40.0f, 22.0f});
        WOKE_CHECK(frame.vertices_added() > 0);
        pill.render(nullptr, Rect{100.0f, 100.0f, 40.0f, 22.0f});
        pill.render(frame.list, Rect{});
    }
    pill.unbind();
    WOKE_CHECK(pill_animation.active_state_count() == 0);
    pill.unbind();

    // ── Module card: geometry, hit targets and the drawer ─────────────────────────
    woke_test::section("widget module card");
    AnimationController card_animation;
    ModuleCard card;
    card.bind(card_animation);

    const Rect card_area{100.0f, 100.0f, 340.0f, 92.0f};
    card.set_area(card_area);
    card.set_content("Zoom Amount", "Smooth camera zoom factor", 'C');
    card.set_enabled(false);
    card.set_drawer_rows(2);

    WOKE_CHECK(card_area.h == ModuleCard::body_height(0.0f)); // grid tile height
    WOKE_CHECK(ModuleCard::body_height(1.0f) == woke::ui::theme::metrics::kRowHeight);
    WOKE_CHECK_STR(card.title(), "Zoom Amount");
    WOKE_CHECK(card.drawer_rows() == 2);
    WOKE_CHECK(nearly(ModuleCard::drawer_height_for(2),
        (ModuleCard::kDrawerPadding * 2.0f) + (2.0f * ModuleCard::kDrawerRowHeight)));
    WOKE_CHECK(ModuleCard::drawer_height_for(0) == 0.0f);

    // Sub-rects read right to left: pill, chevron, chip. A card whose switch and chevron overlap
    // is a card where one of the two clicks is impossible.
    const Rect pill_area = card.pill_area();
    const Rect chevron_area = card.chevron_area();
    const Rect badge_area = card.badge_area();
    WOKE_CHECK(pill_area.w == ModuleCard::kPillWidth);
    WOKE_CHECK(nearly(pill_area.h, woke::ui::theme::metrics::kPillHeight));
    WOKE_CHECK(nearly(pill_area.right(), card_area.right() - ModuleCard::kPaddingX));
    WOKE_CHECK(chevron_area.right() < pill_area.left());
    WOKE_CHECK(badge_area.right() < chevron_area.left());
    WOKE_CHECK(nearly(badge_area.w, ModuleCard::kBadgeWidth));
    WOKE_CHECK(nearly(pill_area.center_y(), card_area.center_y()));
    const Rect drawer = card.drawer_area();
    WOKE_CHECK(nearly(drawer.top(), card_area.bottom()));
    WOKE_CHECK(nearly(drawer.w, card_area.w));
    const Rect drawer_for_two = ModuleCard::drawer_area_for(card_area, 2);
    WOKE_CHECK(nearly(drawer_for_two.h, ModuleCard::drawer_height_for(2)));

    // Clicking the pill or the body toggles; the pill and the body are deliberately one target.
    ModuleCard::Result result;
    hover(input, 800.0f, 800.0f);
    result = card.interact(input);
    WOKE_CHECK(!result.any());

    press(input, pill_area.center_x(), pill_area.center_y());
    result = card.interact(input);
    WOKE_CHECK(result.toggled);
    WOKE_CHECK(!result.expand_toggled);
    WOKE_CHECK(!result.bind_clicked);
    WOKE_CHECK(card.handle_input(input)); // the base contract reports "consumed"

    release(input, pill_area.center_x(), pill_area.center_y());
    result = card.interact(input);
    WOKE_CHECK(!result.any()); // a release never toggles on its own

    // The chevron opens the drawer.
    press(input, chevron_area.center_x(), chevron_area.center_y());
    result = card.interact(input);
    WOKE_CHECK(result.expand_toggled);
    WOKE_CHECK(!result.toggled);

    // The chip click is reported as a bind click and never also toggles the card underneath.
    press(input, badge_area.center_x(), badge_area.center_y());
    result = card.interact(input);
    WOKE_CHECK(result.bind_clicked);
    WOKE_CHECK(!result.toggled);
    WOKE_CHECK(!result.expand_toggled);

    // The drawer reveal and the chevron are one value; a chevron that says "closed" over an open
    // drawer is not a state this component can represent.
    card.set_expanded(true);
    WOKE_CHECK(nearly(card.open_amount(), 0.0f));
    run_frames(card_animation, 40, [&] { card.animate(1.0f / 60.0f); });
    WOKE_CHECK(card.open_amount() > 0.98f);
    card.set_expanded(false);
    run_frames(card_animation, 40, [&] { card.animate(1.0f / 60.0f); });
    WOKE_CHECK(card.open_amount() < 0.02f);

    // Hover is its own value so the card's fill can ease independently of the drawer.
    hover(input, card_area.center_x(), card_area.center_y());
    run_frames(card_animation, 30, [&] { card.animate(1.0f / 60.0f); });
    WOKE_CHECK(card.hover_amount() > 0.9f);

    // The pill follows the module, never its own click: that is what keeps the switch honest.
    card.set_enabled(true);
    run_frames(card_animation, 40, [&] { card.animate(1.0f / 60.0f); });
    WOKE_CHECK(card.pill().amount() > 0.98f);
    card.set_enabled(false);
    run_frames(card_animation, 40, [&] { card.animate(1.0f / 60.0f); });
    WOKE_CHECK(card.pill().amount() < 0.02f);

    {
        HeadlessFrame frame;
        card.set_alpha(1.0f);
        const int glyphs_before = frame.list->VtxBuffer.Size;
        card.render(frame.list, card_area);
        WOKE_CHECK(frame.list->VtxBuffer.Size > glyphs_before);
        card.render(nullptr, card_area);
        card.render(frame.list, Rect{});

        // An open drawer adds its own shell geometry on top of the body.
        const int closed_vertices = frame.list->VtxBuffer.Size;
        card.render(frame.list, card_area);
        const int closed_cost = frame.list->VtxBuffer.Size - closed_vertices;
        card.set_expanded(true);
        run_frames(card_animation, 40, [&] { card.animate(1.0f / 60.0f); });
        const int open_before = frame.list->VtxBuffer.Size;
        card.render(frame.list, card_area);
        WOKE_CHECK((frame.list->VtxBuffer.Size - open_before) > closed_cost);
        card.set_expanded(false);
        run_frames(card_animation, 40, [&] { card.animate(1.0f / 60.0f); });
    }

    card.unbind();
    WOKE_CHECK(card_animation.active_state_count() == 0); // card + pill + chip slots all returned
    card.unbind();

    // ── Sidebar: one definition for the drawn row and the clicked row ─────────────
    woke_test::section("widget sidebar");
    AnimationController sidebar_animation;
    Sidebar sidebar;
    sidebar.bind(sidebar_animation);

    const Rect rail{0.0f, 0.0f, woke::ui::theme::metrics::kSidebarWidth, 556.0f};
    sidebar.set_area(rail);
    sidebar.set_footer("woke.wtf 0.1.0-dev | 240 fps");

    // Twelve rows: six module categories plus the six general pages (§7.1).
    WOKE_CHECK(woke::ui::kSectionCount == 12);
    WOKE_CHECK(woke::ui::kSectionCount == static_cast<std::size_t>(Section::Count));
    WOKE_CHECK(sidebar.selected() == Section::Diagnostics);

    // The first MODULES row sits under the logo block and its heading; the GENERAL heading pushes
    // the rows after the six categories down by its own height plus the section gap.
    const float first_row = Sidebar::row_top(rail, 0);
    WOKE_CHECK(nearly(first_row, Sidebar::kLogoBlockHeight + Sidebar::kSectionLabelHeight));
    WOKE_CHECK(nearly(Sidebar::row_top(rail, 1) - first_row,
        woke::ui::theme::metrics::kNavRowHeight + woke::ui::theme::metrics::kNavRowGap));
    const float general_row = Sidebar::row_top(rail, woke::ui::kModuleCategoryCount);
    WOKE_CHECK(general_row > Sidebar::row_top(rail, woke::ui::kModuleCategoryCount - 1));
    const Rect first_rect = Sidebar::row_rect(rail, 0);
    WOKE_CHECK(nearly(first_rect.left(), Sidebar::kRowInset));
    WOKE_CHECK(nearly(first_rect.w, rail.w - (Sidebar::kRowInset * 2.0f)));
    WOKE_CHECK(nearly(first_rect.h, woke::ui::theme::metrics::kNavRowHeight));
    WOKE_CHECK_STR(Sidebar::heading_for(0), "MODULES");
    WOKE_CHECK_STR(Sidebar::heading_for(woke::ui::kModuleCategoryCount), "GENERAL");
    WOKE_CHECK(Sidebar::heading_for(1) == nullptr);
    WOKE_CHECK(Sidebar::heading_for(woke::ui::kSectionCount) == nullptr);

    // Counter badges: pushed in by the registry, clamped to the table.
    sidebar.set_counts(3, 1, 3); // Movement
    WOKE_CHECK(sidebar.badge_enabled(3) == 1);
    WOKE_CHECK(sidebar.badge_total(3) == 3);
    WOKE_CHECK(sidebar.badge_total(0) == 0); // an untouched row carries no badge
    sidebar.set_counts(woke::ui::kSectionCount, 5, 5);
    WOKE_CHECK(sidebar.badge_total(0) == 0);

    // Hit-testing selects the row under the pointer - and the very same row_rect() drew it.
    hover(input, 20.0f, first_row + 4.0f);
    WOKE_CHECK(!sidebar.handle_input(input)); // hovering consumes nothing
    WOKE_CHECK(!sidebar.take_selection_changed());
    run_frames(sidebar_animation, 30, [&] { sidebar.animate(0.0f); });
    WOKE_CHECK(sidebar.hover_amount(0) > 0.9f);
    WOKE_CHECK(sidebar.hover_amount(1) < 0.05f);

    const Rect combat_row = Sidebar::row_rect(rail, 0);
    press(input, combat_row.center_x(), combat_row.center_y());
    WOKE_CHECK(sidebar.handle_input(input));
    WOKE_CHECK(sidebar.selected() == Section::Combat);
    WOKE_CHECK(sidebar.take_selection_changed());
    WOKE_CHECK(!sidebar.take_selection_changed()); // one-shot

    // A click on the same row again is consumed but is not a change.
    release(input, combat_row.center_x(), combat_row.center_y());
    (void)sidebar.handle_input(input);
    press(input, combat_row.center_x(), combat_row.center_y());
    WOKE_CHECK(sidebar.handle_input(input));
    WOKE_CHECK(!sidebar.take_selection_changed());

    // The GENERAL group selects too (Movement lives at index 3 in the module block, so the
    // general page is reached through its own row).
    const Rect theme_row = Sidebar::row_rect(rail, static_cast<std::size_t>(Section::ThemePage));
    press(input, theme_row.center_x(), theme_row.center_y());
    WOKE_CHECK(sidebar.handle_input(input));
    WOKE_CHECK(sidebar.selected() == Section::ThemePage);

    {
        HeadlessFrame frame;
        sidebar.set_alpha(1.0f);
        sidebar.render(frame.list, rail);
        WOKE_CHECK(frame.vertices_added() > 0);
        sidebar.render(nullptr, rail);
        sidebar.render(frame.list, Rect{});
        sidebar.set_alpha(0.0f);
        const int hidden_before = frame.list->VtxBuffer.Size;
        sidebar.render(frame.list, rail);
        WOKE_CHECK(frame.list->VtxBuffer.Size == hidden_before);
    }
    sidebar.unbind();
    WOKE_CHECK(sidebar_animation.active_state_count() == 0);
    sidebar.unbind();

    // ── Search bar: the filter predicate and the fixed buffer ────────────────────
    woke_test::section("widget search bar");
    AnimationController search_animation;
    SearchBar search;
    search.bind(search_animation);
    search.set_area(Rect{700.0f, 118.0f, 220.0f, 26.0f});

    WOKE_CHECK(search.empty());
    WOKE_CHECK(search.length() == 0);
    WOKE_CHECK(!search.take_changed());
    WOKE_CHECK(search.matches("Zoom Amount")); // an empty query matches everything
    WOKE_CHECK(search.matches(""));
    WOKE_CHECK(!search.matches(nullptr));

    WOKE_CHECK(search.push_char('z'));
    WOKE_CHECK(search.push_char('o'));
    WOKE_CHECK(search.push_char('o'));
    WOKE_CHECK(search.take_changed());
    WOKE_CHECK(!search.take_changed());
    WOKE_CHECK_STR(search.text(), "zoo");
    WOKE_CHECK(search.length() == 3);
    WOKE_CHECK(!search.focused()); // typing does not focus; the click does
    // Case-insensitive containment, on the name and on the description alike.
    WOKE_CHECK(search.matches("Zoom Amount"));
    WOKE_CHECK(search.matches("ZOOM AMOUNT"));
    WOKE_CHECK(!search.matches("Sprint State"));

    // Control characters and non-ASCII never enter the query.
    WOKE_CHECK(!search.push_char(0x1Fu));
    WOKE_CHECK(!search.push_char(0x7Fu));
    WOKE_CHECK(!search.push_char(0x80u));
    WOKE_CHECK_STR(search.text(), "zoo");

    WOKE_CHECK(search.backspace());
    WOKE_CHECK_STR(search.text(), "zo");
    WOKE_CHECK(search.backspace());
    WOKE_CHECK(search.backspace());
    WOKE_CHECK(search.empty());
    WOKE_CHECK(!search.backspace()); // nothing left to delete
    WOKE_CHECK(search.take_changed()); // the deletions above changed the query

    // The buffer is fixed: it clips at capacity and stays usable.
    for (int index = 0; index < 200; ++index) {
        (void)search.push_char('a');
    }
    WOKE_CHECK(search.length() == SearchBar::kCapacity - 1);
    search.clear();
    WOKE_CHECK(search.empty());
    WOKE_CHECK(search.take_changed());
    WOKE_CHECK(!search.take_changed()); // one-shot
    WOKE_CHECK(!search.backspace());

    search.set_text("sprint");
    WOKE_CHECK_STR(search.text(), "sprint");
    WOKE_CHECK(search.matches("Sprint State"));
    WOKE_CHECK(search.take_changed());
    search.set_text("sprint");
    WOKE_CHECK(!search.take_changed()); // setting the same text is not a change
    search.set_text(nullptr);
    WOKE_CHECK(search.empty());
    WOKE_CHECK(search.take_changed());

    // Clicking focuses; clicking away drops focus without consuming the click.
    press(input, 800.0f, 130.0f);
    WOKE_CHECK(search.handle_input(input));
    WOKE_CHECK(search.focused());
    run_frames(search_animation, 30, [&] { search.animate(1.0f / 60.0f); });
    WOKE_CHECK(search.focus_amount() > 0.9f);
    press(input, 400.0f, 400.0f);
    WOKE_CHECK(!search.handle_input(input)); // a click elsewhere drops focus but is not ours
    WOKE_CHECK(!search.focused());
    release(input, 400.0f, 400.0f);
    WOKE_CHECK(!search.handle_input(input)); // a release is not a click

    {
        HeadlessFrame frame;
        search.set_alpha(1.0f);
        search.render(frame.list, Rect{700.0f, 118.0f, 220.0f, 26.0f});
        WOKE_CHECK(frame.vertices_added() > 0);
        // The placeholder and the typed path both draw, and both are safe on empty input.
        search.set_text("zoom");
        const int before = frame.list->VtxBuffer.Size;
        search.render(frame.list, Rect{700.0f, 118.0f, 220.0f, 26.0f});
        WOKE_CHECK(frame.list->VtxBuffer.Size > before);
        search.set_text("");
        search.render(frame.list, Rect{700.0f, 118.0f, 220.0f, 26.0f});
        search.render(nullptr, Rect{700.0f, 118.0f, 220.0f, 26.0f});
        search.render(frame.list, Rect{});
    }
    search.unbind();
    WOKE_CHECK(search_animation.active_state_count() == 0);
    search.unbind();

    // ── Toast pool: capacity, stacking, lifetime and recycling ──────────────────
    woke_test::section("widget notifications");
    AnimationController toast_animation;
    Notifications notifications;
    notifications.bind(toast_animation);
    notifications.set_screen(Rect{0.0f, 0.0f, 1280.0f, 720.0f});

    WOKE_CHECK(!notifications.needs_render());
    WOKE_CHECK(notifications.active_count() == 0);

    for (std::size_t index = 0; index < Notifications::kCapacity; ++index) {
        Toast toast;
        toast.title = "Module";
        toast.message = "Enabled";
        toast.icon = ToastIcon::Success;
        WOKE_CHECK(notifications.push(toast));
    }
    WOKE_CHECK(notifications.active_count() == Notifications::kCapacity);
    WOKE_CHECK(notifications.needs_render());
    WOKE_CHECK(notifications.dropped_count() == 0);

    // A ninth toast is refused rather than overwriting a live one: the user must not miss the
    // message that mattered.
    Toast overflow;
    overflow.title = "Overflow";
    overflow.message = "Should not land";
    overflow.icon = ToastIcon::Warning;
    WOKE_CHECK(!notifications.push(overflow));
    WOKE_CHECK(notifications.dropped_count() == 1);
    WOKE_CHECK(notifications.active_count() == Notifications::kCapacity);

    // The convenience overload fills the standard fields.
    WOKE_CHECK(!notifications.push("Title", "Message")); // pool still full
    WOKE_CHECK(notifications.dropped_count() == 2);

    // Stacking: slot 1 sits exactly one toast-height plus the gap below slot 0. A toast that has
    // not animated yet starts its slide-in exactly one slide distance off the settled corner,
    // which is the "it slides in rather than appearing" property stated as geometry.
    const Rect screen{0.0f, 0.0f, 1280.0f, 720.0f};
    const Rect first = notifications.toast_rect(screen, 0);
    const Rect second = notifications.toast_rect(screen, 1);
    WOKE_CHECK(nearly(first.w, Notifications::kWidth));
    WOKE_CHECK(nearly(first.h, Notifications::kHeight));
    WOKE_CHECK(nearly(notifications.reveal_amount(0), 0.0f));
    WOKE_CHECK(nearly(first.right(),
        screen.right() - Notifications::kMargin + Notifications::kSlideDistance));
    WOKE_CHECK(nearly(second.top() - first.top(), Notifications::kHeight + Notifications::kSpacing));
    WOKE_CHECK(notifications.toast_rect(screen, Notifications::kCapacity).empty()); // out of range

    // Reveal: a toast slides in from the right and settles on the corner.
    run_frames(toast_animation, 30, [&] { notifications.animate(1.0f / 60.0f); });
    WOKE_CHECK(notifications.reveal_amount(0) > 0.9f);
    WOKE_CHECK(nearly(notifications.toast_rect(screen, 0).right(),
        screen.right() - Notifications::kMargin));
    WOKE_CHECK(notifications.elapsed(0) > 0.4f);

    {
        HeadlessFrame frame;
        notifications.set_alpha(1.0f);
        notifications.render(frame.list, screen);
        WOKE_CHECK(frame.vertices_added() > 0);
        notifications.render(nullptr, screen);
        notifications.render(frame.list, Rect{});
    }

    // Clicking a toast dismisses it - the one interaction a stuck toast needs.
    const Rect hit = notifications.toast_rect(screen, 0);
    press(input, hit.center_x(), hit.center_y());
    WOKE_CHECK(notifications.handle_input(input));
    WOKE_CHECK(nearly(notifications.elapsed(0),
        Notifications::kDefaultLifeSeconds)); // life spent: it is now on its way out
    hover(input, 0.0f, 0.0f);
    WOKE_CHECK(!notifications.handle_input(input));

    // Lifetime: an expired toast steers its reveal to zero and is only recycled once it has
    // actually settled, so it can never vanish mid-slide.
    notifications.clear();
    WOKE_CHECK(notifications.active_count() == 0);
    WOKE_CHECK(!notifications.needs_render());

    Toast short_lived;
    short_lived.title = "Config";
    short_lived.message = "Loaded default";
    short_lived.icon = ToastIcon::Info;
    short_lived.life_seconds = 0.5f;
    WOKE_CHECK(notifications.push(short_lived));
    run_frames(toast_animation, 24, [&] { notifications.animate(1.0f / 60.0f); }); // ~0.4 s
    WOKE_CHECK(notifications.active(0));
    WOKE_CHECK(notifications.reveal_amount(0) > 0.9f);
    // Past the life, still sliding out: still active, so the stack has not closed up yet.
    run_frames(toast_animation, 9, [&] { notifications.animate(1.0f / 60.0f); }); // ~0.55 s
    WOKE_CHECK(notifications.active(0));
    WOKE_CHECK(notifications.needs_render());
    // Long enough for the exit to settle: the slot comes back.
    run_frames(toast_animation, 180, [&] { notifications.animate(1.0f / 60.0f); }); // ~3.5 s
    WOKE_CHECK(!notifications.active(0));
    WOKE_CHECK(!notifications.needs_render());
    WOKE_CHECK(notifications.active_count() == 0);

    // An unbound pool still expires toasts (an early-boot frame has no controller yet); it just
    // cannot animate the exit, so it retires them the moment their life is over.
    Notifications unbound;
    WOKE_CHECK(unbound.push("A", "B", 0.25f));
    WOKE_CHECK(unbound.needs_render());
    WOKE_CHECK(unbound.reveal_amount(0) > 0.99f); // unbound reads as fully shown, never invisible
    WOKE_CHECK(nearly(unbound.toast_rect(screen, 0).right(),
        screen.right() - Notifications::kMargin)); // and it still stacks in the corner
    unbound.set_screen(screen);
    unbound.animate(0.5f); // past its life: retired, because there is no exit to animate
    WOKE_CHECK(!unbound.needs_render());

    notifications.clear();
    WOKE_CHECK(toast_animation.active_state_count() == 0); // every reveal slot released
    notifications.unbind();
    notifications.unbind(); // idempotent
    // The pool degrades to "appears and expires" rather than becoming unusable.
    WOKE_CHECK(notifications.push("A", "B"));
    notifications.clear();

    // ── The step's real gate: a populated category, a toggle, and persistence ────
    woke_test::section("widget module integration");

    woke::modules::ModuleManager& registry = woke::modules::manager();
    woke::modules::SprintState sprint;
    woke::modules::ZoomAmount zoom;
    registry.reset();
    WOKE_CHECK(registry.add(&sprint));
    WOKE_CHECK(registry.add(&zoom));

    MemoryStorage storage;
    woke::config::set_storage(&storage);

    // The sidebar's badges are the registry's counters, so a populated category is visible before
    // anybody toggles anything.
    Sidebar live_sidebar;
    AnimationController live_animation;
    live_sidebar.bind(live_animation);
    live_sidebar.set_area(Rect{0.0f, 0.0f, 220.0f, 556.0f});
    for (std::size_t index = 0; index < woke::ui::kModuleCategoryCount; ++index) {
        const woke::modules::Category category = woke::modules::category_from_section(
            static_cast<Section>(index));
        live_sidebar.set_counts(index, registry.category_enabled(category),
            registry.category_total(category));
    }
    const std::size_t movement_index = static_cast<std::size_t>(Section::Movement);
    const std::size_t visual_index = static_cast<std::size_t>(Section::Visual);
    WOKE_CHECK(live_sidebar.badge_total(movement_index) == 1); // Sprint State
    WOKE_CHECK(live_sidebar.badge_total(visual_index) == 1);   // Zoom Amount
    WOKE_CHECK(live_sidebar.badge_enabled(visual_index) == 0);
    WOKE_CHECK(live_sidebar.badge_total(static_cast<std::size_t>(Section::Combat)) == 0);

    // The GUI's own path: build a card for the registry row, click its switch, apply the result
    // to the module exactly as handle_module_cards() does, then ask the worker to persist.
    ModuleCard live_card;
    live_card.bind(live_animation);
    const Rect live_area{240.0f, 380.0f, 340.0f, 92.0f};
    live_card.set_area(live_area);
    live_card.set_content(zoom.name(), zoom.description(), zoom.bind());
    live_card.set_enabled(zoom.enabled());
    WOKE_CHECK_STR(live_card.title(), "Zoom Amount");

    const Rect live_pill = live_card.pill_area();
    press(input, live_pill.center_x(), live_pill.center_y());
    const ModuleCard::Result clicked = live_card.interact(input);
    WOKE_CHECK(clicked.toggled);
    if (clicked.toggled) {
        WOKE_CHECK(zoom.set_enabled(!zoom.enabled()));
        registry.notify_changed();
        woke::config::request_save("default");
    }
    WOKE_CHECK(zoom.enabled());
    WOKE_CHECK(registry.category_enabled(woke::modules::Category::Visual) == 1);
    live_sidebar.set_counts(visual_index, registry.category_enabled(woke::modules::Category::Visual),
        registry.category_total(woke::modules::Category::Visual));
    WOKE_CHECK(live_sidebar.badge_enabled(visual_index) == 1);

    // A functional setting, changed through the erased view a drawer row would use.
    zoom.settings()[0]->set_float(6.5f);
    zoom.settings()[1]->set_enum_index(2);
    WOKE_CHECK(nearly(zoom.settings()[0]->float_value(), 6.5f));
    WOKE_CHECK(zoom.settings()[1]->enum_index() == 2);

    WOKE_CHECK(woke::config::service_saves()); // the worker services the coalesced request
    WOKE_CHECK(storage.exists(woke::config::path_for("default")));
    const std::string document = storage.at(woke::config::path_for("default"));
    WOKE_CHECK(document.find("\"Zoom Amount\"") != std::string::npos);
    WOKE_CHECK(document.find("\"enabled\": true") != std::string::npos);

    // The reinjection boundary: a fresh DLL has an empty registry and freshly constructed
    // modules; the persisted document is what brings the user's state back.
    woke::modules::SprintState sprint2;
    woke::modules::ZoomAmount zoom2;
    registry.reset();
    WOKE_CHECK(registry.add(&sprint2));
    WOKE_CHECK(registry.add(&zoom2));
    WOKE_CHECK(!zoom2.enabled());
    WOKE_CHECK(nearly(zoom2.settings()[0]->float_value(), 3.0f));

    const woke::config::LoadReport loaded = woke::config::load("default");
    WOKE_CHECK(loaded.parsed);
    WOKE_CHECK(loaded.modules_matched == 2);
    WOKE_CHECK(loaded.unknown_modules == 0);
    WOKE_CHECK(loaded.unknown_settings == 0);
    WOKE_CHECK(zoom2.enabled()); // the toggle survived
    WOKE_CHECK(nearly(zoom2.settings()[0]->float_value(), 6.5f));
    WOKE_CHECK(zoom2.settings()[1]->enum_index() == 2);

    // And the reloaded state is what the sidebar and the card report, so the UI cannot disagree
    // with the document.
    live_sidebar.set_counts(visual_index, registry.category_enabled(woke::modules::Category::Visual),
        registry.category_total(woke::modules::Category::Visual));
    WOKE_CHECK(live_sidebar.badge_enabled(visual_index) == 1);
    live_card.set_enabled(zoom2.enabled());
    run_frames(live_animation, 40, [&] { live_card.animate(1.0f / 60.0f); });
    WOKE_CHECK(live_card.pill().amount() > 0.98f);

    {
        HeadlessFrame frame;
        live_card.set_alpha(1.0f);
        live_card.render(frame.list, live_area);
        WOKE_CHECK(frame.vertices_added() > 0);
    }

    live_card.unbind();
    live_sidebar.unbind();
    WOKE_CHECK(live_animation.active_state_count() == 0);

    woke::config::set_storage(nullptr); // release the stack-owned backend before it dies
    registry.reset();
}
