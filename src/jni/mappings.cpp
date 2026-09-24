#include "jni/mappings.h"

#include <exception>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

namespace woke::jni {
namespace {

constexpr std::size_t kMaxWarnings = 24;

using Json = nlohmann::json;

// Warnings are collected, never logged: this translation unit stays free of the logger so
// that it can be linked into the host tests (which run without Windows).
void push_warning(std::vector<std::string>& out, bool& truncated, std::string message) {
    if (truncated) {
        return;
    }
    if (out.size() >= kMaxWarnings) {
        truncated = true;
        out.push_back("mappings: further warnings suppressed");
        return;
    }
    out.push_back(std::move(message));
}

struct Sink {
    std::vector<std::string>* messages;
    bool* truncated;

    void add(std::string message) const { push_warning(*messages, *truncated, std::move(message)); }
};

std::string last_segment(const std::string& path) {
    const std::size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}

std::string join(const std::string& left, const std::string& right) {
    return left + "'" + right + "'";
}

// A document with no explicit "classes" wrapper is still accepted when its values look
// like class entries (variant 5). "stats" cannot trigger this: its "classes" key holds a
// number, not an object.
bool looks_like_class_map(const Json& document) {
    for (auto it = document.begin(); it != document.end(); ++it) {
        const Json& value = it.value();
        if (value.is_object() && (value.contains("intermediary") || value.contains("yarn"))) {
            return true;
        }
    }
    return false;
}

// Member values are objects in the canonical schema and plain identifier strings in
// variant 4. Members are indexed under their Yarn name *and* their intermediary name, so
// either spelling finds the same entry.
void parse_member_map(const Json& source, MemberKind kind, ClassEntry& entry,
    std::size_t& counter, const Sink& sink) {
    if (!source.is_object()) {
        if (!source.is_null()) {
            sink.add("mappings: a member section is not an object and was skipped");
        }
        return;
    }

    auto& target = (kind == MemberKind::Method) ? entry.methods : entry.fields;
    auto& identifiers = (kind == MemberKind::Method) ? entry.method_ids : entry.field_ids;
    for (auto it = source.begin(); it != source.end(); ++it) {
        MemberEntry member;
        member.yarn_name = it.key();

        const Json& value = it.value();
        if (value.is_string()) {
            member.intermediary = value.get<std::string>();
        } else if (value.is_object()) {
            member.intermediary = value.value("intermediary", std::string{});
            if (member.intermediary.empty()) {
                member.intermediary = value.value("id", std::string{});
            }
            member.descriptor = value.value("descriptor", std::string{});
        } else {
            sink.add("mappings: member " + join("", member.yarn_name) + " has an unsupported value");
            continue;
        }

        if (member.intermediary.empty()) {
            sink.add("mappings: member " + join("", member.yarn_name) + " has no intermediary name");
            continue;
        }

        // One canonical entry per member; the obfuscated spelling is an index into it, so
        // both spellings return the same address (and therefore the same cached handle).
        if (target.emplace(member.yarn_name, member).second) {
            ++counter;
            identifiers.emplace(member.intermediary, member.yarn_name);
        }
    }
}

bool build_class_entry(const Json& source, const std::string& key, ClassEntry& entry,
    std::vector<std::string>& aliases, std::size_t& method_counter, std::size_t& field_counter,
    const Sink& sink) {
    entry.yarn_path = source.value("yarn", std::string{});
    if (entry.yarn_path.empty()) {
        entry.yarn_path = source.value("name", std::string{});
    }
    entry.intermediary_path = source.value("intermediary", std::string{});
    if (entry.intermediary_path.empty() && key.find('/') != std::string::npos) {
        // Variant 2: the map key is the intermediary path and the friendly name is optional.
        entry.intermediary_path = key;
    }

    if (entry.yarn_path.empty() && entry.intermediary_path.empty()) {
        sink.add("mappings: class " + join("", key) + " has neither a yarn nor an intermediary name");
        return false;
    }
    if (entry.yarn_path.empty()) {
        entry.yarn_path = entry.intermediary_path;
    }

    entry.simple_name = last_segment(entry.yarn_path);
    entry.intermediary_simple = last_segment(entry.intermediary_path);

    aliases = {key, entry.simple_name, entry.yarn_path, entry.intermediary_path,
        entry.intermediary_simple};
    const auto explicit_aliases = source.find("aliases");
    if (explicit_aliases != source.end() && explicit_aliases->is_array()) {
        for (const Json& alias : *explicit_aliases) {
            if (alias.is_string()) {
                aliases.push_back(alias.get<std::string>());
            }
        }
    }

    parse_member_map(source.value("methods", Json::object()), MemberKind::Method, entry,
        method_counter, sink);
    parse_member_map(source.value("fields", Json::object()), MemberKind::Field, entry,
        field_counter, sink);
    return true;
}

// The identifiers the core systems depend on, each with the value the blueprint
// documents (§5.4). A silent upstream rename therefore shows up as a boot warning instead
// of as a mystery in-game failure, and the host test asserts this same table against the
// asset that ships in the repository.
struct AnchorSpec {
    const char* class_alias;
    const char* member; // nullptr for a class-only anchor
    MemberKind kind;
    const char* expected; // nullptr when no identifier comparison applies
};

constexpr AnchorSpec kAnchors[] = {
    {"class_310", nullptr, MemberKind::Method, nullptr},
    {"class_746", nullptr, MemberKind::Method, nullptr},
    {"class_638", nullptr, MemberKind::Method, nullptr},
    {"class_315", nullptr, MemberKind::Method, nullptr},
    {"class_310", "getInstance", MemberKind::Method, "method_1551"},
    {"class_310", "player", MemberKind::Field, "field_1724"},
    {"class_310", "world", MemberKind::Field, "field_1687"},
};

} // namespace

void Mappings::reset() {
    classes_.clear();
    aliases_.clear();
    warnings_.clear();
    warnings_truncated_ = false;
    method_count_ = 0;
    field_count_ = 0;
    version_.clear();
    source_.clear();
}

void Mappings::warn(std::string message) {
    push_warning(warnings_, warnings_truncated_, std::move(message));
}

bool Mappings::load_file(const char* path) {
    reset();
    if (path == nullptr || *path == '\0') {
        warn("mappings: no asset path was resolved");
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        warn(std::string("mappings: cannot open '") + path + "'");
        return false;
    }

    const std::string text(
        (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (!parse(text)) {
        return false;
    }
    return loaded();
}

bool Mappings::parse(std::string_view json_text) {
    reset();

    Sink sink{&warnings_, &warnings_truncated_};
    if (json_text.empty()) {
        sink.add("mappings: asset is empty");
        return false;
    }

    try {
        const Json document = Json::parse(json_text.begin(), json_text.end(), nullptr, false);
        if (document.is_discarded()) {
            sink.add("mappings: asset is not valid JSON");
            return false;
        }
        if (!document.is_object()) {
            sink.add("mappings: asset must be a JSON object");
            return false;
        }

        version_ = document.value("version", std::string{});
        source_ = document.value("source", std::string{});

        const Json* container = nullptr;
        const auto classes = document.find("classes");
        if (classes != document.end() && (classes->is_object() || classes->is_array())) {
            container = &(*classes);
        } else if (looks_like_class_map(document)) {
            container = &document;
        }
        if (container == nullptr) {
            sink.add("mappings: asset has no usable \"classes\" section");
            return false;
        }

        const auto insert_entry = [&](const Json& source, const std::string& key) {
            if (!source.is_object()) {
                sink.add("mappings: class " + join("", key) + " is not an object");
                return;
            }

            ClassEntry entry;
            std::vector<std::string> aliases;
            std::size_t methods = 0;
            std::size_t fields = 0;
            if (!build_class_entry(source, key, entry, aliases, methods, fields, sink)) {
                return;
            }

            const std::string canonical = entry.simple_name.empty() ? key : entry.simple_name;
            // Pointers into an unordered_map are stable across rehash, so aliases stay
            // valid for the lifetime of the registry.
            ClassEntry& stored = classes_.emplace(canonical, std::move(entry)).first->second;
            method_count_ += methods;
            field_count_ += fields;
            for (const std::string& alias : aliases) {
                if (!alias.empty()) {
                    aliases_.emplace(alias, &stored); // first registration wins
                }
            }
        };

        if (container->is_array()) {
            for (const Json& element : *container) {
                // Variant 3: an array of entries, each carrying its own name.
                insert_entry(element, element.is_object() ? element.value("name", std::string{}) : std::string{});
            }
        } else {
            for (auto it = container->begin(); it != container->end(); ++it) {
                insert_entry(it.value(), it.key());
            }
        }

        return loaded();
    } catch (const std::exception& error) {
        sink.add(std::string("mappings: parse aborted - ") + error.what());
        return false;
    } catch (...) {
        sink.add("mappings: parse aborted by an unknown error");
        return false;
    }
}

const ClassEntry* Mappings::find_class(std::string_view alias) const noexcept {
    const auto found = aliases_.find(alias);
    return found == aliases_.end() ? nullptr : found->second;
}

const MemberEntry* Mappings::find_member(
    std::string_view class_alias, std::string_view member, MemberKind kind) const noexcept {
    const ClassEntry* entry = find_class(class_alias);
    if (entry == nullptr) {
        return nullptr;
    }

    const auto& members = (kind == MemberKind::Method) ? entry->methods : entry->fields;
    const auto found = members.find(member);
    if (found != members.end()) {
        return &found->second;
    }

    // Not the Yarn name: try the intermediary spelling before giving up.
    const auto& identifiers = (kind == MemberKind::Method) ? entry->method_ids : entry->field_ids;
    const auto alias = identifiers.find(member);
    if (alias == identifiers.end()) {
        return nullptr;
    }

    const auto canonical = members.find(alias->second);
    return canonical == members.end() ? nullptr : &canonical->second;
}

const MemberEntry* Mappings::find_method(
    std::string_view class_alias, std::string_view member) const noexcept {
    return find_member(class_alias, member, MemberKind::Method);
}

const MemberEntry* Mappings::find_field(
    std::string_view class_alias, std::string_view member) const noexcept {
    return find_member(class_alias, member, MemberKind::Field);
}

AnchorStatus Mappings::verify_anchors() const {
    AnchorStatus status;
    status.checks.reserve(std::size(kAnchors));

    for (const AnchorSpec& anchor : kAnchors) {
        AnchorCheck check;
        check.class_alias = anchor.class_alias;
        check.member = (anchor.member != nullptr) ? anchor.member : "";
        check.kind = anchor.kind;
        check.expected_intermediary = (anchor.expected != nullptr) ? anchor.expected : "";

        if (anchor.member == nullptr) {
            check.resolved = (find_class(anchor.class_alias) != nullptr);
            check.matches_expected = check.resolved;
        } else {
            const MemberEntry* entry = find_member(anchor.class_alias, anchor.member, anchor.kind);
            check.resolved = (entry != nullptr);
            check.matches_expected = check.resolved
                && (check.expected_intermediary.empty()
                    || entry->intermediary == check.expected_intermediary);
        }

        if (check.resolved) {
            ++status.resolved;
        } else {
            ++status.failed;
        }
        if (check.resolved && !check.matches_expected) {
            ++status.mismatched;
        }
        status.checks.push_back(std::move(check));
    }

    return status;
}

} // namespace woke::jni
