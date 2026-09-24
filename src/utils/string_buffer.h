#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace woke::util {

// Fixed-capacity, stack-allocated text buffer.
//
// Every hot-path string in woke.wtf lives in one of these. The render and tick loops
// must never touch the heap, so text is formatted into caller-owned storage and
// truncated rather than grown. Truncation is silent by design: a clipped log line or
// HUD label must never be able to stall a frame or crash the game.
template <std::size_t Capacity>
class FixedString {
public:
    static_assert(Capacity >= 2, "FixedString must hold at least one character plus the terminator");
    static constexpr std::size_t capacity = Capacity;

    FixedString() noexcept = default;

    void clear() noexcept {
        length_ = 0;
        buffer_[0] = '\0';
    }

    void assign(std::string_view text) noexcept {
        const std::size_t take = text.size() < (Capacity - 1) ? text.size() : (Capacity - 1);
        if (take > 0) {
            std::memcpy(buffer_, text.data(), take);
        }
        length_ = take;
        buffer_[length_] = '\0';
    }

    void append(std::string_view text) noexcept {
        const std::size_t room = (Capacity - 1) - length_;
        const std::size_t take = text.size() < room ? text.size() : room;
        if (take > 0) {
            std::memcpy(buffer_ + length_, text.data(), take);
        }
        length_ += take;
        buffer_[length_] = '\0';
    }

    // printf-style formatting, in place. Arguments must be trivial (POD/char*) values.
    template <typename... Args>
    void format(const char* format, Args... args) noexcept {
        const int written = std::snprintf(buffer_, Capacity, format, args...);
        apply_length(written);
    }

    // va_list variant, used by the logger so that a log line is formatted exactly once.
    void format_v(const char* format, va_list args) noexcept {
        const int written = std::vsnprintf(buffer_, Capacity, format, args);
        apply_length(written);
    }

    [[nodiscard]] const char* c_str() const noexcept { return buffer_; }
    [[nodiscard]] std::size_t size() const noexcept { return length_; }
    [[nodiscard]] bool empty() const noexcept { return length_ == 0; }
    [[nodiscard]] std::string_view view() const noexcept { return std::string_view(buffer_, length_); }

    FixedString& operator+=(std::string_view text) noexcept {
        append(text);
        return *this;
    }

private:
    // snprintf reports the length it *would* have written; clamp it to what fits.
    void apply_length(int written) noexcept {
        if (written < 0) {
            clear();
            return;
        }
        const std::size_t produced = static_cast<std::size_t>(written);
        length_ = produced < (Capacity - 1) ? produced : (Capacity - 1);
        buffer_[length_] = '\0';
    }

    char buffer_[Capacity] = {};
    std::size_t length_ = 0;
};

template <std::size_t Capacity, typename... Args>
void fmt_into(FixedString<Capacity>& out, const char* format, Args... args) noexcept {
    out.format(format, args...);
}

} // namespace woke::util
