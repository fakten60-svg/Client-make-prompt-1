#include "ui/hud.h"

#include <array>
#include <cmath>
#include <cstring>

#include "core/version.h"
#include "modules/category.h"
#include "modules/module_manager.h"
#include "ui/theme.h"
#include "utils/string_buffer.h"

namespace woke::ui::hud {
namespace {

using modules::BaseModule;

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

bool active(const Frame& frame) noexcept {
    return frame.watermark || frame.arraylist || frame.crosshair
        || (frame.velocity_chip && frame.world_live) || (frame.trajectory && frame.world_live);
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
    if (frame.watermark) {
        draw_watermark(draw_list, frame, alpha);
    }
    if (frame.velocity_chip && frame.world_live) {
        draw_velocity_chip(draw_list, frame, alpha);
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
