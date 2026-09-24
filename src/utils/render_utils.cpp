#include "utils/render_utils.h"

#include <cfloat>
#include <string>
#include <string_view>

#include "utils/string_buffer.h"

namespace woke::ui::draw {
namespace {

// Ellipsis buffer for text_clipped(): far more than any label the overlay draws, and fixed so the
// clipping path never allocates (§6.3).
constexpr std::size_t kClipBufferSize = 192;

ImVec2 to_vec(float x, float y) noexcept {
    return ImVec2(x, y);
}

} // namespace

ImU32 to_im_u32(const Rgba& color) noexcept {
    const auto channel = [](float value) noexcept {
        const float clamped = util::clamp01(value);
        return static_cast<unsigned int>((clamped * 255.0f) + 0.5f);
    };
    return IM_COL32(channel(color.r), channel(color.g), channel(color.b), channel(color.a));
}

ImVec4 to_im_vec4(const Rgba& color) noexcept {
    return ImVec4(color.r, color.g, color.b, color.a);
}

void rounded_rect(ImDrawList* draw_list, const Rect& rect, const Rgba& fill, float rounding,
    ImDrawFlags flags) noexcept {
    if (draw_list == nullptr || rect.empty() || fill.a <= 0.0f) {
        return;
    }
    draw_list->AddRectFilled(to_vec(rect.left(), rect.top()), to_vec(rect.right(), rect.bottom()),
        to_im_u32(fill), rounding, flags);
}

void border_stroke(ImDrawList* draw_list, const Rect& rect, const Rgba& color, float rounding,
    float thickness) noexcept {
    if (draw_list == nullptr || rect.empty() || color.a <= 0.0f || thickness <= 0.0f) {
        return;
    }
    // 1.92.8+ parameter order: (p_min, p_max, col, rounding, thickness, flags). The older
    // (…, rounding, flags, thickness) form is deleted under IMGUI_DISABLE_OBSOLETE_FUNCTIONS.
    draw_list->AddRect(to_vec(rect.left(), rect.top()), to_vec(rect.right(), rect.bottom()),
        to_im_u32(color), rounding, thickness, 0);
}

void shadow_rect(ImDrawList* draw_list, const Rect& rect, float spread, const Rgba& color) noexcept {
    if (draw_list == nullptr || rect.empty() || spread <= 0.0f || color.a <= 0.0f) {
        return;
    }

    const ImU32 opaque = to_im_u32(color);
    const ImU32 transparent = IM_COL32(0, 0, 0, 0);

    // Top edge: transparent along the outer edge, full strength where it meets the window.
    draw_list->AddRectFilledMultiColor(to_vec(rect.left() - spread, rect.top() - spread),
        to_vec(rect.right() + spread, rect.top()), transparent, transparent, opaque, opaque);
    // Bottom edge.
    draw_list->AddRectFilledMultiColor(to_vec(rect.left() - spread, rect.bottom()),
        to_vec(rect.right() + spread, rect.bottom() + spread), opaque, opaque, transparent,
        transparent);
    // Left edge.
    draw_list->AddRectFilledMultiColor(to_vec(rect.left() - spread, rect.top()),
        to_vec(rect.left(), rect.bottom()), transparent, opaque, opaque, transparent);
    // Right edge.
    draw_list->AddRectFilledMultiColor(to_vec(rect.right(), rect.top()),
        to_vec(rect.right() + spread, rect.bottom()), opaque, transparent, transparent, opaque);
}

void gradient_fill_v(
    ImDrawList* draw_list, const Rect& rect, const Rgba& top, const Rgba& bottom) noexcept {
    if (draw_list == nullptr || rect.empty()) {
        return;
    }
    const ImU32 top_color = to_im_u32(top);
    const ImU32 bottom_color = to_im_u32(bottom);
    draw_list->AddRectFilledMultiColor(to_vec(rect.left(), rect.top()),
        to_vec(rect.right(), rect.bottom()), top_color, top_color, bottom_color, bottom_color);
}

void gradient_fill_h(
    ImDrawList* draw_list, const Rect& rect, const Rgba& left, const Rgba& right) noexcept {
    if (draw_list == nullptr || rect.empty()) {
        return;
    }
    const ImU32 left_color = to_im_u32(left);
    const ImU32 right_color = to_im_u32(right);
    draw_list->AddRectFilledMultiColor(to_vec(rect.left(), rect.top()),
        to_vec(rect.right(), rect.bottom()), left_color, right_color, right_color, left_color);
}

void circle(
    ImDrawList* draw_list, float center_x, float center_y, float radius, const Rgba& fill) noexcept {
    if (draw_list == nullptr || radius <= 0.0f || fill.a <= 0.0f) {
        return;
    }
    draw_list->AddCircleFilled(to_vec(center_x, center_y), radius, to_im_u32(fill));
}

void ring(ImDrawList* draw_list, float center_x, float center_y, float radius, const Rgba& color,
    float thickness) noexcept {
    if (draw_list == nullptr || radius <= 0.0f || color.a <= 0.0f || thickness <= 0.0f) {
        return;
    }
    draw_list->AddCircle(to_vec(center_x, center_y), radius, to_im_u32(color), 0, thickness);
}

void line(ImDrawList* draw_list, float ax, float ay, float bx, float by, const Rgba& color,
    float thickness) noexcept {
    if (draw_list == nullptr || color.a <= 0.0f || thickness <= 0.0f) {
        return;
    }
    draw_list->AddLine(to_vec(ax, ay), to_vec(bx, by), to_im_u32(color), thickness);
}

float text_width(const char* text, float font_size) noexcept {
    if (text == nullptr || *text == '\0') {
        return 0.0f;
    }
    if (font_size > 0.0f) {
        return ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text).x;
    }
    return ImGui::CalcTextSize(text).x;
}

float text_height(float font_size) noexcept {
    if (font_size > 0.0f) {
        return font_size;
    }
    return ImGui::GetTextLineHeight();
}

void text(ImDrawList* draw_list, float x, float y, const char* label, const Rgba& color,
    float font_size) noexcept {
    if (draw_list == nullptr || label == nullptr || *label == '\0' || color.a <= 0.0f) {
        return;
    }
    const ImU32 packed = to_im_u32(color);
    if (font_size > 0.0f) {
        draw_list->AddText(ImGui::GetFont(), font_size, to_vec(x, y), packed, label);
        return;
    }
    draw_list->AddText(to_vec(x, y), packed, label);
}

void text_in(ImDrawList* draw_list, const Rect& area, const char* label, const Rgba& color,
    Align align, float font_size) noexcept {
    if (draw_list == nullptr || label == nullptr || *label == '\0' || area.empty()) {
        return;
    }

    const float width = text_width(label, font_size);
    float x = area.left();
    if (align == Align::Center) {
        x = area.center_x() - (width * 0.5f);
    } else if (align == Align::Right) {
        x = area.right() - width;
    }
    const float y = area.center_y() - (text_height(font_size) * 0.5f);
    text(draw_list, x, y, label, color, font_size);
}

void text_clipped(ImDrawList* draw_list, const Rect& area, const char* text, const Rgba& color,
    Align align, float font_size) noexcept {
    if (draw_list == nullptr || text == nullptr || *text == '\0' || area.empty()) {
        return;
    }

    if (text_width(text, font_size) <= area.w) {
        text_in(draw_list, area, text, color, align, font_size);
        return;
    }

    // Binary search the longest prefix that still fits next to the ellipsis. Measuring the whole
    // string once already told us it does not fit, so the search starts at the length that is
    // certain to fit and narrows from there.
    const std::size_t length = std::char_traits<char>::length(text);
    util::FixedString<kClipBufferSize> candidate;
    std::size_t fits = 0;
    std::size_t low = 1;
    std::size_t high = length;
    while (low <= high) {
        const std::size_t middle = low + ((high - low) / 2);
        candidate.clear();
        candidate.assign(std::string_view(text, middle));
        candidate.append("...");
        if (text_width(candidate.c_str(), font_size) <= area.w) {
            fits = middle;
            low = middle + 1;
        } else if (middle == 0) {
            break;
        } else {
            high = middle - 1;
        }
    }

    util::FixedString<kClipBufferSize> clipped;
    if (fits == 0) {
        // Even "..." alone is wider than the area; draw the ellipsis and let the clip rect win.
        clipped.assign("...");
    } else {
        clipped.assign(std::string_view(text, fits));
        clipped.append("...");
    }

    text_in(draw_list, area, clipped.c_str(), color, align, font_size);
}

} // namespace woke::ui::draw
