#include "test_harness.h"

#include "jni/mappings.h"

// The mappings registry is the one component whose failure mode is "the game updated
// overnight". It is therefore tested the same way it is written: exhaustively, with no
// exception escaping on any input, and - for the asset that actually ships - against the
// exact intermediary identifiers the blueprint documents (§5.4).

namespace {

using woke::jni::MemberEntry;
using woke::jni::MemberKind;
using woke::jni::Mappings;

// Canonical schema: an object keyed by friendly name.
constexpr const char* kCanonical = R"json({
  "version": "1.21.11",
  "namespace": "intermediary",
  "source": "net.fabricmc:yarn:1.21.11+build.6",
  "classes": {
    "MinecraftClient": {
      "yarn": "net/minecraft/client/MinecraftClient",
      "intermediary": "net/minecraft/class_310",
      "aliases": ["MinecraftClient", "class_310", "net/minecraft/class_310"],
      "methods": {
        "getInstance": { "intermediary": "method_1551", "descriptor": "()Lnet/minecraft/class_310;" }
      },
      "fields": {
        "player": { "intermediary": "field_1724", "descriptor": "Lnet/minecraft/class_746;" }
      }
    }
  }
})json";

// Variant 2: keyed by intermediary path, friendly name optional.
constexpr const char* kKeyedByIntermediary = R"json({
  "classes": {
    "net/minecraft/class_310": {
      "name": "net/minecraft/client/MinecraftClient",
      "methods": { "getInstance": "method_1551" }
    }
  }
})json";

// Variant 3: an array of entries, each carrying its own name.
constexpr const char* kArrayOfEntries = R"json({
  "classes": [
    {
      "name": "net/minecraft/client/MinecraftClient",
      "intermediary": "net/minecraft/class_310",
      "methods": { "getInstance": { "intermediary": "method_1551", "descriptor": "()V" } }
    }
  ]
})json";

// Variant 5: no "classes" wrapper at all.
constexpr const char* kUnwrapped = R"json({
  "MinecraftClient": { "intermediary": "net/minecraft/class_310" }
})json";

} // namespace

void test_mappings_parser() {
    woke_test::section("mappings registry - canonical schema");

    {
        Mappings mappings;
        WOKE_CHECK(mappings.parse(kCanonical));
        WOKE_CHECK(mappings.loaded());
        WOKE_CHECK(mappings.class_count() == 1);
        WOKE_CHECK(mappings.method_count() == 1);
        WOKE_CHECK(mappings.field_count() == 1);
        WOKE_CHECK(mappings.warnings().empty());
        WOKE_CHECK_STR(mappings.version().c_str(), "1.21.11");
        WOKE_CHECK_STR(mappings.source().c_str(), "net.fabricmc:yarn:1.21.11+build.6");

        // Every documented alias resolves to the same entry.
        const char* aliases[] = {
            "MinecraftClient", "class_310", "net/minecraft/class_310",
            "net/minecraft/client/MinecraftClient"};
        for (const char* alias : aliases) {
            const auto* entry = mappings.find_class(alias);
            WOKE_CHECK(entry != nullptr);
            if (entry != nullptr) {
                WOKE_CHECK_STR(entry->simple_name.c_str(), "MinecraftClient");
                WOKE_CHECK_STR(entry->intermediary_path.c_str(), "net/minecraft/class_310");
            }
        }

        const MemberEntry* method = mappings.find_method("class_310", "getInstance");
        WOKE_CHECK(method != nullptr);
        if (method != nullptr) {
            WOKE_CHECK_STR(method->intermediary.c_str(), "method_1551");
            WOKE_CHECK_STR(method->descriptor.c_str(), "()Lnet/minecraft/class_310;");
        }

        // Members are addressable by their Yarn name and by their intermediary name.
        const MemberEntry* by_intermediary = mappings.find_method("class_310", "method_1551");
        WOKE_CHECK(by_intermediary == method);

        const MemberEntry* field = mappings.find_field("MinecraftClient", "player");
        WOKE_CHECK(field != nullptr);
        if (field != nullptr) {
            WOKE_CHECK_STR(field->intermediary.c_str(), "field_1724");
        }

        // Wrong kind, unknown class and unknown member all fail closed.
        WOKE_CHECK(mappings.find_method("class_310", "player") == nullptr);
        WOKE_CHECK(mappings.find_field("class_310", "getInstance") == nullptr);
        WOKE_CHECK(mappings.find_class("class_999") == nullptr);
        WOKE_CHECK(mappings.find_method("class_310", "noSuchMember") == nullptr);

        // The anchor table agrees with this document.
        const auto anchors = mappings.verify_anchors();
        WOKE_CHECK(anchors.checks.size() == 7);
        WOKE_CHECK(anchors.resolved == 3); // only class_310, getInstance and player are present
        WOKE_CHECK(anchors.failed == 4);
        WOKE_CHECK(anchors.mismatched == 0);
        WOKE_CHECK(!anchors.all_ok());
    }

    woke_test::section("mappings registry - tolerated schema variants");

    {
        Mappings mappings;
        WOKE_CHECK(mappings.parse(kKeyedByIntermediary));
        const auto* entry = mappings.find_class("class_310");
        WOKE_CHECK(entry != nullptr);
        if (entry != nullptr) {
            WOKE_CHECK_STR(entry->simple_name.c_str(), "MinecraftClient");
            WOKE_CHECK_STR(entry->yarn_path.c_str(), "net/minecraft/client/MinecraftClient");
        }
        // Variant 4 in the same document: a plain identifier string carries no descriptor.
        const MemberEntry* method = mappings.find_method("class_310", "getInstance");
        WOKE_CHECK(method != nullptr);
        if (method != nullptr) {
            WOKE_CHECK_STR(method->intermediary.c_str(), "method_1551");
            WOKE_CHECK(method->descriptor.empty());
        }
    }

    {
        Mappings mappings;
        WOKE_CHECK(mappings.parse(kArrayOfEntries));
        const auto* entry = mappings.find_class("MinecraftClient");
        WOKE_CHECK(entry != nullptr);
        WOKE_CHECK(mappings.find_method("class_310", "getInstance") != nullptr);
        WOKE_CHECK(mappings.class_count() == 1);
    }

    {
        Mappings mappings;
        WOKE_CHECK(mappings.parse(kUnwrapped));
        WOKE_CHECK(mappings.class_count() == 1);
        WOKE_CHECK(mappings.find_class("class_310") != nullptr);
    }

    woke_test::section("mappings registry - defensive failures");

    {
        Mappings mappings;
        const char* rubbish[] = {
            "",
            "not json at all",
            "[1, 2, 3]",
            "{}",
            "{\"classes\": 5}",
            "{\"classes\": {\"Broken\": {\"methods\": {\"a\": {}}}}}",
        };
        for (const char* document : rubbish) {
            WOKE_CHECK(!mappings.parse(document));
            WOKE_CHECK(!mappings.loaded());
            // A rejected document must leave nothing behind to look up.
            WOKE_CHECK(mappings.find_class("class_1") == nullptr);
        }
    }

    {
        // A class with neither a yarn nor an intermediary name is reported and skipped, and
        // the rest of the document still loads.
        Mappings mappings;
        const bool parsed = mappings.parse(R"json({
          "classes": {
            "Nameless": { "methods": { "x": "method_1" } },
            "Good": { "intermediary": "net/minecraft/class_7" }
          }
        })json");
        WOKE_CHECK(parsed);
        WOKE_CHECK(mappings.class_count() == 1);
        WOKE_CHECK(!mappings.warnings().empty());
        WOKE_CHECK(mappings.find_class("class_7") != nullptr);
    }

    {
        Mappings mappings;
        WOKE_CHECK(!mappings.load_file("this/path/does/not/exist/mappings.json"));
        WOKE_CHECK(!mappings.loaded());
        WOKE_CHECK(!mappings.warnings().empty());
        WOKE_CHECK(!mappings.warnings_truncated());
    }

    woke_test::section("mappings asset - shipped document");

    {
        Mappings mappings;
        WOKE_CHECK(mappings.load_file(WOKE_MAPPINGS_ASSET));
        WOKE_CHECK(mappings.loaded());
        WOKE_CHECK_STR(mappings.version().c_str(), "1.21.11");
        WOKE_CHECK(mappings.class_count() == 40); // 8d added ItemCooldownManager
        WOKE_CHECK(mappings.method_count() > 2000);
        WOKE_CHECK(mappings.field_count() > 1000);

        // The contract the core depends on: every documented anchor resolves, and each one
        // still points at the identifier the blueprint records.
        const auto anchors = mappings.verify_anchors();
        WOKE_CHECK(anchors.checks.size() == 7);
        WOKE_CHECK(anchors.resolved == 7);
        WOKE_CHECK(anchors.failed == 0);
        WOKE_CHECK(anchors.mismatched == 0);
        WOKE_CHECK(anchors.all_ok());

        // Spot checks of the members the JVM bridge resolves at boot.
        const MemberEntry* get_instance = mappings.find_method("class_310", "getInstance");
        WOKE_CHECK(get_instance != nullptr);
        if (get_instance != nullptr) {
            WOKE_CHECK_STR(get_instance->descriptor.c_str(), "()Lnet/minecraft/class_310;");
        }

        const MemberEntry* yaw = mappings.find_method("class_1297", "getYaw");
        WOKE_CHECK(yaw != nullptr);
        if (yaw != nullptr) {
            WOKE_CHECK_STR(yaw->descriptor.c_str(), "(F)F");
        }

        const MemberEntry* health = mappings.find_method("class_1309", "getHealth");
        WOKE_CHECK(health != nullptr);
        if (health != nullptr) {
            WOKE_CHECK_STR(health->descriptor.c_str(), "()F");
        }

        // Record values are not method_XXXX/field_XXXX, which is exactly why nothing in the
        // client may key off a name prefix.
        const auto* vec = mappings.find_class("class_243");
        WOKE_CHECK(vec != nullptr);
        if (vec != nullptr) {
            const MemberEntry* x = mappings.find_field("class_243", "x");
            WOKE_CHECK(x != nullptr);
            if (x != nullptr) {
                WOKE_CHECK_STR(x->intermediary.c_str(), "field_1352");
                WOKE_CHECK_STR(x->descriptor.c_str(), "D");
            }
        }
    }

    woke_test::section("mappings registry - MemberKind coverage");
    WOKE_CHECK(static_cast<int>(MemberKind::Method) != static_cast<int>(MemberKind::Field));
}
