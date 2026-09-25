#pragma once

// Settings (blueprint §3.4, §4.2).
//
// A setting is a named, self-describing value that knows how to move itself to and from a JSON
// value. Everything downstream - the config engine, the GUI widgets - operates on the type-erased
// `Setting` view, so adding a module with new settings requires zero config code: persistence is a
// property of the setting, not a registration step (§4.2's auto-binding invariant).
//
// Storage is fixed-size: a module owns its settings by value, and no setting ever allocates.
// Tolerance rules from §4.2: a missing key keeps the default, a wrong-type key is ignored, and
// nothing here can throw - a malformed config must not take the game down.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <nlohmann/json.hpp>

#include "utils/math_utils.h"

namespace woke::settings {

enum class Kind : std::uint8_t {
    Bool = 0,
    Slider,
    Enum,
};

[[nodiscard]] constexpr const char* kind_label(Kind kind) noexcept {
    switch (kind) {
    case Kind::Bool:
        return "bool";
    case Kind::Slider:
        return "slider";
    case Kind::Enum:
        return "enum";
    default:
        return "?";
    }
}

// The type-erased view the config engine and the GUI widgets operate on. Construction goes through
// the concrete types below; the base is neither copyable nor directly constructible, which keeps
// the union's active member a compile-time fact rather than a runtime risk.
class Setting {
public:
    Setting(const Setting&) = delete;
    Setting& operator=(const Setting&) = delete;

    [[nodiscard]] const char* name() const noexcept { return name_; }
    [[nodiscard]] const char* description() const noexcept { return description_; }
    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    void clear_dirty() noexcept { dirty_ = false; }

    // ── Type-erased reads. A wrong-kind read returns the zero value rather than asserting: a
    // widget can only end up on a mismatched setting through a registration error that the
    // module's own compile-time registration would already have caught.

    [[nodiscard]] bool bool_value() const noexcept {
        return kind_ == Kind::Bool ? storage_.bool_.value : false;
    }

    [[nodiscard]] float float_value() const noexcept {
        return kind_ == Kind::Slider ? storage_.slider_.value : 0.0f;
    }

    [[nodiscard]] std::size_t enum_index() const noexcept {
        return kind_ == Kind::Enum ? storage_.enum_.index : 0;
    }

    [[nodiscard]] std::string_view enum_label() const noexcept {
        if (kind_ != Kind::Enum || storage_.enum_.index >= storage_.enum_.count) {
            return {};
        }
        return storage_.enum_.labels[storage_.enum_.index];
    }

    [[nodiscard]] std::string_view enum_label(std::size_t index) const noexcept {
        if (kind_ != Kind::Enum || index >= storage_.enum_.count) {
            return {};
        }
        return storage_.enum_.labels[index];
    }

    // Bounds and option count, so a widget can render a slider or cycle an enum through the
    // erased view alone - no downcast, and a wrong-kind read is still the zero value.
    [[nodiscard]] std::size_t enum_count() const noexcept {
        return kind_ == Kind::Enum ? storage_.enum_.count : 0;
    }

    [[nodiscard]] float float_minimum() const noexcept {
        return kind_ == Kind::Slider ? storage_.slider_.minimum : 0.0f;
    }

    [[nodiscard]] float float_maximum() const noexcept {
        return kind_ == Kind::Slider ? storage_.slider_.maximum : 0.0f;
    }

    // ── Mutation paths. Every one sets the dirty flag, and only on an actual change, so the
    // config engine never rewrites the file for a no-op (§4.4's persist-dirty flag).

    void set_bool(bool value) noexcept {
        if (kind_ != Kind::Bool || storage_.bool_.value == value) {
            return;
        }
        storage_.bool_.value = value;
        dirty_ = true;
    }

    void set_float(float value) noexcept {
        if (kind_ != Kind::Slider) {
            return;
        }
        const float clamped = util::clamp(value, storage_.slider_.minimum, storage_.slider_.maximum);
        if (storage_.slider_.value == clamped) {
            return;
        }
        storage_.slider_.value = clamped;
        dirty_ = true;
    }

    void set_enum_index(std::size_t index) noexcept {
        if (kind_ != Kind::Enum || storage_.enum_.index == index || index >= storage_.enum_.count) {
            return;
        }
        storage_.enum_.index = index;
        dirty_ = true;
    }

    // ── JSON round trip. to_json writes the live value; from_json applies a value only when it is
    // present *and* of the right kind, which is §4.2's tolerance contract.

    void to_json(nlohmann::json& out) const noexcept {
        try {
            switch (kind_) {
            case Kind::Bool:
                out = storage_.bool_.value;
                break;
            case Kind::Slider:
                // Rounded so slider churn cannot fill the file with float noise.
                out = std::round(storage_.slider_.value * 1000.0f) / 1000.0f;
                break;
            case Kind::Enum:
                out = std::string_view(storage_.enum_.labels[storage_.enum_.index]);
                break;
            default:
                break;
            }
        } catch (...) {
            // out stays untouched: a serialization hiccup is a config-file problem, not a crash.
        }
    }

    // Returns true when a value was applied (and the dirty flag set).
    [[nodiscard]] bool from_json(const nlohmann::json& value) noexcept {
        try {
            switch (kind_) {
            case Kind::Bool:
                if (value.is_boolean()) {
                    set_bool(value.get<bool>());
                    return true;
                }
                return false;
            case Kind::Slider:
                if (value.is_number()) {
                    set_float(value.get<float>());
                    return true;
                }
                return false;
            case Kind::Enum:
                if (value.is_string()) {
                    const std::string label = value.get<std::string>();
                    for (std::size_t index = 0; index < storage_.enum_.count; ++index) {
                        if (label == storage_.enum_.labels[index]) {
                            set_enum_index(index);
                            return true;
                        }
                    }
                }
                return false;
            default:
                return false;
            }
        } catch (...) {
            return false;
        }
    }

protected:
    struct BoolInit {
        bool value;
    };

    struct SliderInit {
        float value;
        float minimum;
        float maximum;
        float step;
    };

    struct EnumInit {
        const char* const* labels;
        std::size_t count;
        std::size_t index;
    };

    explicit Setting(const char* name, const char* description, BoolInit init) noexcept
        : name_(name), description_(description), kind_(Kind::Bool), storage_{.bool_ = init} {}

    explicit Setting(const char* name, const char* description, SliderInit init) noexcept
        : name_(name), description_(description), kind_(Kind::Slider), storage_{.slider_ = init} {}

    explicit Setting(const char* name, const char* description, EnumInit init) noexcept
        : name_(name), description_(description), kind_(Kind::Enum), storage_{.enum_ = init} {}

    ~Setting() = default;

protected:
    union Storage {
        BoolInit bool_;
        SliderInit slider_;
        EnumInit enum_;
    };

    const char* name_ = nullptr;
    const char* description_ = nullptr;
    Kind kind_;
    Storage storage_;
    bool dirty_ = false;
};

class BoolSetting final : public Setting {
public:
    BoolSetting(const char* name, const char* description, bool default_value) noexcept
        : Setting(name, description, BoolInit{default_value}) {}

    [[nodiscard]] bool value() const noexcept { return bool_value(); }
    void set(bool value) noexcept { set_bool(value); }
    void toggle() noexcept { set_bool(!bool_value()); }
};

class SliderSetting final : public Setting {
public:
    SliderSetting(const char* name, const char* description, float default_value, float minimum,
        float maximum, float step) noexcept
        : Setting(name, description,
              SliderInit{util::clamp(default_value, minimum, maximum), minimum, maximum, step}) {}

    [[nodiscard]] float value() const noexcept { return float_value(); }
    [[nodiscard]] float minimum() const noexcept { return storage_.slider_.minimum; }
    [[nodiscard]] float maximum() const noexcept { return storage_.slider_.maximum; }
    [[nodiscard]] float step() const noexcept { return storage_.slider_.step; }
    void set(float value) noexcept { set_float(value); }
};

class EnumSetting final : public Setting {
public:
    template <std::size_t N>
    EnumSetting(const char* name, const char* description,
        const char* const (&labels)[N], std::size_t default_index = 0) noexcept
        : Setting(name, description,
              EnumInit{labels, N, default_index < N ? default_index : 0}) {}

    [[nodiscard]] std::size_t count() const noexcept { return storage_.enum_.count; }
    [[nodiscard]] std::string_view value() const noexcept { return enum_label(); }
};

} // namespace woke::settings
