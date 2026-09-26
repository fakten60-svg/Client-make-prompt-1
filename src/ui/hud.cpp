#include "ui/hud.h"

#include <array>
#include <cmath>
#include <cstring>

#include "core/version.h"
#include "modules/category.h"
#include "modules/mace/smash_damage.h"
#include "modules/mace/smash_potential.h"
#include "modules/module_manager.h"
#include "ui/theme.h"
#include "utils/string_buffer.h"

namespace woke::ui::hud {
namespace {

using modules::BaseModule;
namespace mace = woke::modules::mace;

constexpr float kMargin = 8.0f;
constexpr float kRowHeight = 18.0f;
constexpr float kRowGap = 2.0f;
constexpr float kRowPadding = 8.0f;
constexpr float kAccentBarWidth = 2.0f;
constexpr float kMarkerRadius = 2.5f;
constexpr float kOutlineExtra = 1.5f;
constexpr float kChipHeight = 20.0f;
constexpr float kWatermarkHeight = 20.0f;
constexpr float kRowAdvance = 24.0f;

// The unit table the chip formats with, index-matched to Movement/Velocity Display's enum. Kept
// here rather than included from the module so the renderer stays a pure function of the frame.
constexpr const char* kSpeedUnits[] = {"m/s", "km/h", "mph"};
// Index-matched to kSpeedUnits: one table means the label and the factor can never disagree.
constexpr double kSpeedFactors[] = {1.0, 3.6, 2.2369362920544};
constexpr std::size_t kSpeedUnitCount = sizeof(kSpeedUnits) / sizeof(kSpeedUnits[0]);
static_assert(sizeof(kSpeedFactors) / sizeof(kSpeedFactors[0]) == kSpeedUnitCount,
    "the speed label and factor tables must stay index-matched");

// One arraylist entry: the module's name, its measured width and its category accent. The width
// is measured once, here, because the row is sized from it and the draw call is reused.
struct Row {
    const char* name = nullptr;
    float width = 0.0f;
    Rgba accent = util::from_hex(0xFFFFFF);
};

// The chip plate the left column is built from: card fill, accent bar, one line of text. Every
// step-8 chip composes it so the column reads as one client rather than five widgets. The frame
// is not consulted - a chip is always left-aligned at the margin - but it stays in the signature
// so every chip's call site reads the same.
Rect chip_plate(const Frame& /*frame*/, float y, float width) noexcept {
    return Rect{kMargin, y, width + (kRowPadding * 2.0f), kChipHeight};
}

void draw_chip(ImDrawList* draw_list, const Rect& plate, const char* text, const Rgba& accent,
    float alpha) noexcept {
    draw::rounded_rect(draw_list, plate, util::scale_alpha(theme::color::kCard, alpha * 0.85f),
        theme::metrics::kFrameRounding);
    draw::rounded_rect(draw_list, Rect{plate.x, plate.y, kAccentBarWidth, plate.h},
        util::scale_alpha(accent, alpha), theme::metrics::kFrameRounding);
    draw::text_in(draw_list, plate.offset(kRowPadding, 0.0f), text,
        util::scale_alpha(theme::color::kText, alpha), draw::Align::Left);
}

// The y position of the next left-column chip: directly under the watermark and the velocity
// chip when those are showing, so the column stays a column.
float left_column_y(const Frame& frame) noexcept {
    float y = kMargin;
    if (frame.watermark) {
        y += kRowAdvance;
    }
    if (frame.velocity_chip && frame.world_live) {
        y += kRowAdvance;
    }
    return y;
}

std::size_t collect_rows(std::array<Row, modules::ModuleManager::kMaxModules>& rows) noexcept {
    const modules::ModuleManager& registry = modules::manager();
    std::size_t count = 0;
    for (std::size_t index = 0; index < registry.count() && count < rows.size(); ++index) {
        const BaseModule* module = registry.at(index);
        if (module == nullptr || !module->enabled()) {
            continue;
        }
        Row& row = rows[count++];
        row.name = module->name();
        row.width = draw::text_width(row.name);
        row.accent = theme::category_accent(modules::category_to_section(module->category()));
    }
    return count;
}

// Insertion sort over a handful of rows: no allocation, no scratch buffer, and the two orderings
// differ only in the comparison (§7.6's "sorted by name width").
void sort_rows(std::array<Row, modules::ModuleManager::kMaxModules>& rows, std::size_t count,
    bool by_length) noexcept {
    for (std::size_t index = 1; index < count; ++index) {
        Row current = rows[index];
        std::size_t hole = index;
        while (hole > 0) {
            const Row& previous = rows[hole - 1];
            const bool out_of_order = by_length ? (previous.width < current.width)
                                                : (std::strcmp(previous.name, current.name) > 0);
            if (!out_of_order) {
                break;
            }
            rows[hole] = previous;
            --hole;
        }
        rows[hole] = current;
    }
}

void draw_watermark(ImDrawList* draw_list, const Frame& frame, float alpha) noexcept {
    util::FixedString<64> text;
    text.format("%s %s  |  %d fps", version::kClientName, version::kVersion,
        static_cast<int>(frame.frames_per_second + 0.5f));

    const Rect plate{kMargin, kMargin, draw::text_width(text.c_str()) + (kRowPadding * 2.0f),
        kWatermarkHeight};

    draw::rounded_rect(draw_list, plate, util::scale_alpha(theme::color::kCard, alpha * 0.85f),
        theme::metrics::kFrameRounding);
    draw::rounded_rect(draw_list,
        Rect{plate.x, plate.y, kAccentBarWidth, plate.h},
        util::scale_alpha(theme::color::kAccent, alpha), theme::metrics::kFrameRounding);
    draw::text_in(draw_list, plate.offset(kRowPadding, 0.0f), text.c_str(),
        util::scale_alpha(theme::color::kText, alpha), draw::Align::Left);
}

// The speed readout, top-left under the watermark (roadmap step 8). Same plate as the watermark so
// the two chips read as one column rather than two unrelated badges.
void draw_velocity_chip(ImDrawList* draw_list, const Frame& frame, float alpha) noexcept {
    const double meters_per_second = std::sqrt(
        (frame.velocity.x * frame.velocity.x) + (frame.velocity.z * frame.velocity.z));
    // An out-of-range unit index falls back to the first entry rather than to zero, so a stale
    // index can never show a stopped player.
    const std::size_t index = frame.velocity_units < kSpeedUnitCount ? frame.velocity_units : 0;

    util::FixedString<32> text;
    text.format("%.1f %s", meters_per_second * kSpeedFactors[index], kSpeedUnits[index]);

    const float y = frame.watermark ? (kMargin + kRowAdvance) : kMargin;
    const Rect plate{kMargin, y, draw::text_width(text.c_str()) + (kRowPadding * 2.0f), kChipHeight};

    draw::rounded_rect(draw_list, plate, util::scale_alpha(theme::color::kCard, alpha * 0.85f),
        theme::metrics::kFrameRounding);
    draw::rounded_rect(draw_list, Rect{plate.x, plate.y, kAccentBarWidth, plate.h},
        util::scale_alpha(theme::color::kAccent, alpha), theme::metrics::kFrameRounding);
    draw::text_in(draw_list, plate.offset(kRowPadding, 0.0f), text.c_str(),
        util::scale_alpha(theme::color::kText, alpha), draw::Align::Left);
}

// ── Step 8: Combat and Mace readouts ─────────────────────────────────────────────

// The left-column chips share a running cursor: each one draws at `column_y` and advances it,
// so enabling three readouts stacks a column instead of stacking three chips on one spot.

void draw_target_card(ImDrawList* draw_list, const Frame& frame, float alpha,
    float& column_y) noexcept {
    const float scale = frame.target_scale > 0.25f ? frame.target_scale : 0.25f;

    util::FixedString<96> text;
    if (frame.target.name.empty()) {
        text.format("%.1f m", frame.target.distance);
    } else {
        text.format("%s  -  %.1f m", frame.target.name.c_str(), frame.target.distance);
    }

    // The health bar is the card's second row when the target has one; a non-living target
    // (its max health read as zero) draws a single name-distance row instead.
    const bool has_health = frame.target.max_health > 0.0f;
    const float line_height = draw::text_height() * scale;
    const float bar_height = has_health ? 4.0f * scale : 0.0f;
    const float width = (draw::text_width(text.c_str()) > (60.0f * scale)
                                ? draw::text_width(text.c_str())
                                : (60.0f * scale))
        + (kRowPadding * 2.0f);
    const float height = (kRowPadding * 2.0f) + line_height + bar_height
        + (has_health ? (2.0f * scale) : 0.0f);

    Rect plate{kMargin, kMargin, width, height};
    if (frame.target_position == 0) {
        // Under the crosshair, following the game's own nametag convention.
        plate.x = frame.screen.center_x() - (width * 0.5f);
        plate.y = frame.screen.center_y() + (24.0f * scale);
    } else {
        plate.y = column_y;
        column_y += height + kRowGap;
    }

    draw::rounded_rect(draw_list, plate, util::scale_alpha(theme::color::kCard, alpha * 0.85f),
        theme::metrics::kFrameRounding);
    draw::rounded_rect(draw_list, Rect{plate.x, plate.y, kAccentBarWidth, plate.h},
        util::scale_alpha(theme::color::kAccent, alpha), theme::metrics::kFrameRounding);
    draw::text_clipped(draw_list, plate.inset(kRowPadding).slice_top(line_height), text.c_str(),
        util::scale_alpha(theme::color::kText, alpha), draw::Align::Left);

    if (has_health) {
        const Rect bar_area =
            Rect{plate.x + kRowPadding, plate.bottom() - bar_height - kRowPadding,
                plate.w - (kRowPadding * 2.0f), bar_height};
        draw::rounded_rect(draw_list, bar_area,
            util::scale_alpha(theme::color::kSeparator, alpha), 2.0f);
        const float fraction = frame.target.max_health > 0.0f
            ? util::clamp01(frame.target.health / frame.target.max_health)
            : 0.0f;
        const float absorption_fraction = frame.target.max_health > 0.0f
            ? util::clamp01(
                (frame.target.health + frame.target.absorption) / frame.target.max_health)
            : 0.0f;
        draw::rounded_rect(draw_list,
            Rect{bar_area.x, bar_area.y, bar_area.w * fraction, bar_area.h},
            util::scale_alpha(theme::color::kTrafficRed, alpha), 2.0f);
        if (absorption_fraction > fraction) {
            draw::rounded_rect(draw_list,
                Rect{bar_area.x + (bar_area.w * fraction), bar_area.y,
                    bar_area.w * (absorption_fraction - fraction), bar_area.h},
                util::scale_alpha(theme::color::kTrafficYellow, alpha), 2.0f);
        }
    }
}

void draw_cooldown_bar(ImDrawList* draw_list, const Frame& frame, float alpha) noexcept {
    const float progress = util::clamp01(frame.cooldown_progress);
    const Rgba color = util::scale_alpha(frame.cooldown_color, alpha);
    const float center_x = frame.screen.center_x();
    const float center_y = frame.screen.center_y();

    if (frame.cooldown_style == 1) {
        // The arc: twelve segments around the crosshair. A real arc path would cost a
        // tessellation per frame; twelve short chords read identically at this radius.
        constexpr int kSegments = 12;
        constexpr float kRadius = 16.0f;
        constexpr float kFullAngle = 6.2831853f; // 2pi
        const int filled = static_cast<int>(progress * static_cast<float>(kSegments));
        for (int index = 0; index < kSegments; ++index) {
            const float angle0 =
                (static_cast<float>(index) / static_cast<float>(kSegments)) * kFullAngle;
            const float angle1 =
                (static_cast<float>(index + 1) / static_cast<float>(kSegments)) * kFullAngle;
            const Rgba segment_color = index < filled
                ? color
                : util::scale_alpha(theme::color::kSeparator, alpha);
            draw::line(draw_list, center_x + (std::cos(angle0) * kRadius),
                center_y + (std::sin(angle0) * kRadius),
                center_x + (std::cos(angle1) * kRadius),
                center_y + (std::sin(angle1) * kRadius), segment_color, 2.0f);
        }
        return;
    }

    // The bar: a fixed-width track under the crosshair with a filled leading edge.
    constexpr float kBarWidth = 44.0f;
    constexpr float kBarHeight = 3.0f;
    const Rect track{center_x - (kBarWidth * 0.5f), center_y + 14.0f, kBarWidth, kBarHeight};
    draw::rounded_rect(draw_list, track, util::scale_alpha(theme::color::kSeparator, alpha), 1.5f);
    if (progress > 0.001f) {
        draw::rounded_rect(
            draw_list, Rect{track.x, track.y, track.w * progress, track.h}, color, 1.5f);
    }
}

void draw_reach_chip(ImDrawList* draw_list, const Frame& frame, float alpha,
    float& column_y) noexcept {
    util::FixedString<32> text;
    text.format("reach %.2f", frame.reach_blocks);
    const Rect plate = chip_plate(frame, column_y, draw::text_width(text.c_str()));
    draw_chip(draw_list, plate, text.c_str(), theme::color::kAccent, alpha);
    column_y += kRowAdvance;
}

// The two session-counter chips. Drawn as one function because they share a format; the combat
// counters win the slot when both are enabled, which the §8 ordering (Combat before Mace) also
// implies. Counters survive a world change: they are the session's data, not the world's.
void draw_counter_chip(ImDrawList* draw_list, const Frame& frame, float alpha,
    float& column_y) noexcept {
    if (frame.combat_counters) {
        util::FixedString<64> text;
        text.format("S %u  H %u  W %u", frame.counter_swings, frame.counter_hits,
            frame.counter_wasted);
        const Rect plate = chip_plate(frame, column_y, draw::text_width(text.c_str()));
        draw_chip(draw_list, plate, text.c_str(), theme::color::kAccentAlt, alpha);
        column_y += kRowAdvance;
    }
    if (frame.mace_counters) {
        util::FixedString<64> text;
        text.format("M S %u  H %u  SM %u", frame.mace_swing_count, frame.mace_hit_count,
            frame.mace_smash_count);
        const Rect plate = chip_plate(frame, column_y, draw::text_width(text.c_str()));
        draw_chip(draw_list, plate, text.c_str(), theme::color::kTrafficYellow, alpha);
        column_y += kRowAdvance;
    }
}

void draw_smash_chip(ImDrawList* draw_list, const Frame& frame, float alpha,
    float& column_y) noexcept {
    const mace::SmashDamage projected =
        mace::project_smash(frame.fall_distance, frame.smash_enhanced);

    util::FixedString<48> text;
    text.format("smash %.1f%s", projected.total, projected.smash_ready ? "  READY" : "");

    // Colour by damage band: normal, notable, lethal. The thresholds are the module's; the
    // swatches are the theme's.
    Rgba accent = theme::color::kTrafficYellow;
    switch (mace::SmashPotential::band_for(projected.total)) {
    case 2:
        accent = theme::color::kTrafficRed;
        break;
    case 1:
        accent = theme::color::kTrafficGreen;
        break;
    default:
        accent = theme::color::kTrafficYellow;
        break;
    }

    const Rect plate = chip_plate(frame, column_y, draw::text_width(text.c_str()));
    draw_chip(draw_list, plate, text.c_str(), accent, alpha);
    column_y += kRowAdvance;
}

// ── Step 8c: Misc / Movement / Spear chips ────────────────────────────────────

void draw_friend_chip(ImDrawList* draw_list, const Frame& frame, float alpha,
    float& column_y) noexcept {
    util::FixedString<32> text;
    text.format("friends %u", frame.friend_count);
    const Rect plate = chip_plate(frame, column_y, draw::text_width(text.c_str()));
    draw_chip(draw_list, plate, text.c_str(), theme::color::kAccent, alpha);
    column_y += kRowAdvance;
}

void draw_safe_walk_chip(ImDrawList* draw_list, const Frame& frame, float alpha,
    float& column_y) noexcept {
    util::FixedString<32> text;
    text.format("safe walk %s", frame.safe_walk_engaged ? "ON" : "off");
    const Rect plate = chip_plate(frame, column_y, draw::text_width(text.c_str()));
    draw_chip(draw_list, plate, text.c_str(),
        frame.safe_walk_engaged ? theme::color::kTrafficGreen : theme::color::kTextMuted, alpha);
    column_y += kRowAdvance;
}

void draw_riptide_chip(ImDrawList* draw_list, const Frame& frame, float alpha,
    float& column_y) noexcept {
    const char* state = frame.riptide_engaged
        ? "ACTIVE"
        : (frame.riptide_trident ? "READY" : "no trident");
    util::FixedString<40> text;
    text.format("riptide %s", state);
    const Rect plate = chip_plate(frame, column_y, draw::text_width(text.c_str()));
    draw_chip(draw_list, plate, text.c_str(),
        frame.riptide_engaged ? theme::color::kAccentAlt : theme::color::kAccent, alpha);
    column_y += kRowAdvance;
}

void draw_trident_chip(ImDrawList* draw_list, const Frame& frame, float alpha,
    float& column_y) noexcept {
    util::FixedString<32> text;
    text.format("trident %.0f%%", static_cast<double>(util::clamp01(frame.trident_progress) * 100.0f));
    const Rect plate = chip_plate(frame, column_y, draw::text_width(text.c_str()));
    draw_chip(draw_list, plate, text.c_str(), frame.trident_color, alpha);
    column_y += kRowAdvance;
}

void draw_loyalty_chip(ImDrawList* draw_list, const Frame& frame, float alpha,
    float& column_y) noexcept {
    util::FixedString<48> text;
    if (frame.loyalty_tracking) {
        text.format("loyalty %.1fs out", frame.loyalty_seconds);
    } else {
        text.format("loyalty trip %.1fs", frame.loyalty_last_trip);
    }
    const Rect plate = chip_plate(frame, column_y, draw::text_width(text.c_str()));
    draw_chip(draw_list, plate, text.c_str(), theme::color::kTrafficYellow, alpha);
    column_y += kRowAdvance;
}

void draw_wind_charge_chip(ImDrawList* draw_list, const Frame& frame, float alpha,
    float& column_y) noexcept {
    util::FixedString<32> text;
    text.format("wind charge %.0f%%",
        static_cast<double>(util::clamp01(frame.wind_charge_progress) * 100.0f));
    const Rect plate = chip_plate(frame, column_y, draw::text_width(text.c_str()));
    draw_chip(draw_list, plate, text.c_str(), frame.wind_charge_color, alpha);
    column_y += kRowAdvance;
}

// Four translucent edge quads, the shadow's inverse: a flash *is* an unshadow. Drawn on the
// same frame, so it needs no second pass and costs eight vertices.
void draw_smash_flash(ImDrawList* draw_list, const Frame& frame, float alpha) noexcept {
    const float strength = util::clamp01(frame.flash_intensity);
    if (strength <= 0.0f) {
        return;
    }
    const Rgba color = util::scale_alpha(accent_color_for(0), alpha * strength * 0.35f);
    constexpr float kEdge = 26.0f;
    const Rect& screen = frame.screen;

    draw_list->AddRectFilled(ImVec2(screen.left(), screen.top()),
        ImVec2(screen.right(), screen.top() + kEdge), draw::to_im_u32(color));
    draw_list->AddRectFilled(ImVec2(screen.left(), screen.bottom() - kEdge),
        ImVec2(screen.right(), screen.bottom()), draw::to_im_u32(color));
    draw_list->AddRectFilled(ImVec2(screen.left(), screen.top()),
        ImVec2(screen.left() + kEdge, screen.bottom()), draw::to_im_u32(color));
    draw_list->AddRectFilled(ImVec2(screen.right() - kEdge, screen.top()),
        ImVec2(screen.right(), screen.bottom()), draw::to_im_u32(color));
}

void draw_arraylist(ImDrawList* draw_list, const Frame& frame, float alpha) noexcept {
    std::array<Row, modules::ModuleManager::kMaxModules> rows{};
    const std::size_t count = collect_rows(rows);
    if (count == 0) {
        return;
    }
    sort_rows(rows, count, frame.arraylist_by_length);

    float y = kMargin;
    for (std::size_t index = 0; index < count; ++index) {
        const Row& row = rows[index];
        const float width = row.width + (kRowPadding * 2.0f);
        const Rect plate{frame.screen.right() - kMargin - width, y, width, kRowHeight};

        draw::rounded_rect(draw_list, plate, util::scale_alpha(theme::color::kCard, alpha * 0.75f),
            theme::metrics::kFrameRounding);
        draw::rounded_rect(draw_list, Rect{plate.right() - kAccentBarWidth, plate.y, kAccentBarWidth, plate.h},
            util::scale_alpha(row.accent, alpha), theme::metrics::kFrameRounding);
        draw::text_in(draw_list, plate.inset(kRowPadding), row.name,
            util::scale_alpha(theme::color::kText, alpha), draw::Align::Left);

        y += kRowHeight + kRowGap;
    }
}

void draw_crosshair(ImDrawList* draw_list, const Frame& frame, float alpha) noexcept {
    const CrosshairStyle& style = frame.crosshair_style;
    const visual::CrosshairGeometry geometry =
        visual::crosshair_geometry(style.shape, style.size, style.gap, style.thickness);
    const float center_x = frame.screen.center_x();
    const float center_y = frame.screen.center_y();
    const Rgba color = util::scale_alpha(style.color, alpha);
    const Rgba outline = util::scale_alpha(theme::color::kShadow, alpha);

    for (std::size_t index = 0; index < geometry.arm_count; ++index) {
        const visual::Segment& arm = geometry.arms[index];
        // A dark under-stroke first, so the crosshair reads against a bright sky as well as a
        // dark cave. Two draw calls per arm is the whole cost.
        if (style.outline) {
            draw::line(draw_list, center_x + arm.x0, center_y + arm.y0, center_x + arm.x1,
                center_y + arm.y1, outline, geometry.thickness + kOutlineExtra);
        }
        draw::line(draw_list, center_x + arm.x0, center_y + arm.y0, center_x + arm.x1,
            center_y + arm.y1, color, geometry.thickness);
    }

    if (geometry.dot) {
        if (style.outline) {
            draw::circle(draw_list, center_x, center_y, geometry.dot_radius + kOutlineExtra * 0.5f,
                outline);
        }
        draw::circle(draw_list, center_x, center_y, geometry.dot_radius, color);
    }
    if (geometry.ring) {
        draw::ring(draw_list, center_x, center_y, geometry.ring_radius, color, geometry.thickness);
    }
}

void draw_trajectory(ImDrawList* draw_list, const Frame& frame, float alpha) noexcept {
    // Samples behind the near plane are dropped, so a path that passes behind the player draws
    // two separate runs rather than a smear across the screen.
    std::array<visual::ScreenPoint, visual::kMaxTrajectoryPoints> visible{};
    visual::View view{};
    view.eye = frame.eye;
    view.yaw_degrees = frame.yaw_degrees;
    view.pitch_degrees = frame.pitch_degrees;
    view.vertical_fov_degrees = frame.vertical_fov_degrees;
    view.viewport_width = frame.screen.w;
    view.viewport_height = frame.screen.h;

    std::array<game::Vec3d, visual::kMaxTrajectoryPoints> path{};
    const std::size_t path_count = visual::predict_trajectory(
        frame.eye, frame.velocity, frame.trajectory_style.params, path);

    std::size_t visible_count = 0;
    for (std::size_t index = 0; index < path_count; ++index) {
        visual::ScreenPoint projected{};
        if (!visual::project_to_screen(view, path[index], projected)) {
            continue;
        }
        if (projected.x < frame.screen.left() || projected.x > frame.screen.right()
            || projected.y < frame.screen.top() || projected.y > frame.screen.bottom()) {
            // Off screen: kept out of `visible` so the polyline breaks instead of drawing a long
            // chord back across the viewport.
            continue;
        }
        visible[visible_count++] = projected;
    }

    const Rgba color = util::scale_alpha(frame.trajectory_style.color, alpha);
    for (std::size_t index = 1; index < visible_count; ++index) {
        draw::line(draw_list, visible[index - 1].x, visible[index - 1].y, visible[index].x,
            visible[index].y, color, 1.5f);
    }
    if (visible_count > 0) {
        const visual::ScreenPoint& end = visible[visible_count - 1];
        draw::circle(draw_list, end.x, end.y, kMarkerRadius, color);
    }
}

} // namespace

// Index-matched to the step-8 modules' colour enums (Attack Cooldown, Smash Flash). One table here
// means a re-theme stays one file. Defined at namespace scope because it is part of the renderer's
// public vocabulary (step8_frame.cpp hands the resolved colour to the Frame), not a private helper.
Rgba accent_color_for(const std::size_t index) noexcept {
    switch (index) {
    case 1:
        return theme::color::kText;
    case 2:
        return theme::color::kTrafficGreen;
    case 3:
        return theme::color::kTrafficRed;
    case 4:
        return theme::color::kAccentAlt;
    default:
        return theme::color::kAccent;
    }
}

bool active(const Frame& frame) noexcept {
    // The counter chips are session data, so they draw with no world live - everything else in
    // the step-8 set gates on world_live like the trajectory does.
    const bool combat_reads = frame.target_card || frame.cooldown_bar || frame.reach_chip
        || frame.smash_chip || frame.smash_flash;
    // The step-8c spear chips are game reads (a trident/riptide state), so they gate on world_live
    // like the trajectory; the friend and safe-walk chips are local/session facts and do not. The
    // wind-charge chip is a game read like the spear chips (an item cooldown exists per world).
    const bool spear_reads = frame.riptide_chip || frame.trident_chip || frame.loyalty_chip;
    const bool local_chips = frame.friend_chip || frame.safe_walk_chip;
    return frame.watermark || frame.arraylist || frame.crosshair
        || (frame.velocity_chip && frame.world_live) || (frame.trajectory && frame.world_live)
        || (combat_reads && frame.world_live) || frame.combat_counters || frame.mace_counters
        || local_chips || (spear_reads && frame.world_live)
        || (frame.wind_charge_chip && frame.world_live);
}

void render(ImDrawList* draw_list, const Frame& frame) noexcept {
    if (draw_list == nullptr || frame.screen.empty()) {
        return;
    }
    const float alpha = util::clamp01(frame.alpha);
    if (alpha <= 0.0f) {
        return;
    }

    if (frame.trajectory && frame.world_live) {
        draw_trajectory(draw_list, frame, alpha);
    }
    if (frame.crosshair) {
        draw_crosshair(draw_list, frame, alpha);
    }
    if (frame.cooldown_bar && frame.world_live) {
        draw_cooldown_bar(draw_list, frame, alpha);
    }
    if (frame.smash_flash && frame.world_live) {
        draw_smash_flash(draw_list, frame, alpha);
    }
    if (frame.watermark) {
        draw_watermark(draw_list, frame, alpha);
    }
    if (frame.velocity_chip && frame.world_live) {
        draw_velocity_chip(draw_list, frame, alpha);
    }
    // The left-column readouts share one cursor so any combination stacks a column.
    float column_y = left_column_y(frame);
    if (frame.target_card && frame.world_live && frame.target.valid) {
        draw_target_card(draw_list, frame, alpha, column_y);
    }
    if (frame.reach_chip && frame.world_live) {
        draw_reach_chip(draw_list, frame, alpha, column_y);
    }
    if (frame.smash_chip && frame.world_live) {
        draw_smash_chip(draw_list, frame, alpha, column_y);
    }
    if (frame.combat_counters || frame.mace_counters) {
        draw_counter_chip(draw_list, frame, alpha, column_y);
    }
    if (frame.friend_chip) {
        draw_friend_chip(draw_list, frame, alpha, column_y);
    }
    if (frame.safe_walk_chip) {
        draw_safe_walk_chip(draw_list, frame, alpha, column_y);
    }
    if (frame.riptide_chip && frame.world_live) {
        draw_riptide_chip(draw_list, frame, alpha, column_y);
    }
    if (frame.trident_chip && frame.world_live) {
        draw_trident_chip(draw_list, frame, alpha, column_y);
    }
    if (frame.loyalty_chip && frame.world_live) {
        draw_loyalty_chip(draw_list, frame, alpha, column_y);
    }
    if (frame.wind_charge_chip && frame.world_live) {
        draw_wind_charge_chip(draw_list, frame, alpha, column_y);
    }
    if (frame.arraylist) {
        draw_arraylist(draw_list, frame, alpha);
    }
}

std::size_t project_trajectory(
    const Frame& frame, std::array<visual::ScreenPoint, visual::kMaxTrajectoryPoints>& out) noexcept {
    visual::View view{};
    view.eye = frame.eye;
    view.yaw_degrees = frame.yaw_degrees;
    view.pitch_degrees = frame.pitch_degrees;
    view.vertical_fov_degrees = frame.vertical_fov_degrees;
    view.viewport_width = frame.screen.w;
    view.viewport_height = frame.screen.h;

    std::array<game::Vec3d, visual::kMaxTrajectoryPoints> path{};
    const std::size_t path_count =
        visual::predict_trajectory(frame.eye, frame.velocity, frame.trajectory_style.params, path);

    std::size_t count = 0;
    for (std::size_t index = 0; index < path_count && count < out.size(); ++index) {
        visual::ScreenPoint projected{};
        if (visual::project_to_screen(view, path[index], projected)) {
            out[count++] = projected;
        }
    }
    return count;
}

} // namespace woke::ui::hud
