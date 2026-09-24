#include "test_harness.h"

// Module-system tests (roadmap step 5).
//
// The step's gate is "two dummy modules toggle and persist across reinjection", so the test does
// exactly that: registers the example modules, toggles them, saves a config, tears the registry
// down (the reinjection boundary), rebuilds it, and asserts the state came back. The config
// engine's file IO is injectable, so the whole round trip runs in memory on Linux.

#include <map>
#include <string>
#include <vector>

#include "core/config.h"
#include "modules/examples.h"
#include "modules/module_manager.h"

namespace {

[[nodiscard]] bool util_nearly(float a, float b) noexcept {
    return (a > b ? a - b : b - a) <= 0.001f;
}

// In-memory store standing in for configs/<name>.json across a "reinject".
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
        ++writes_;
        return true;
    }

    [[nodiscard]] bool exists(std::string_view path) const {
        return files_.find(std::string(path)) != files_.end();
    }

    std::map<std::string, std::string> files_;
    int writes_ = 0;
};

using woke::config::LoadReport;
using woke::modules::BaseModule;
using woke::modules::Category;

// Registers the example modules into the process-wide registry - the real one the config engine
// and the GUI walk. reset() models the reinjection boundary: a fresh DLL has an empty registry.
void fresh_registry(woke::modules::SprintState& sprint, woke::modules::ZoomAmount& zoom) {
    woke::modules::ModuleManager& manager = woke::modules::manager();
    manager.reset();
    (void)manager.add(&sprint);
    (void)manager.add(&zoom);
}

} // namespace

void test_module_system() {
    woke_test::section("module registry");

    woke::modules::ModuleManager& manager = woke::modules::manager();
    woke::modules::SprintState sprint;
    woke::modules::ZoomAmount zoom;
    fresh_registry(sprint, zoom);

    WOKE_CHECK(manager.count() == 2);
    WOKE_CHECK(manager.find("Sprint State") == &sprint);
    WOKE_CHECK(manager.find("Zoom Amount") == &zoom);
    WOKE_CHECK(manager.find("nope") == nullptr);
    WOKE_CHECK(manager.at(0) != nullptr && manager.at(1) != nullptr);
    WOKE_CHECK(manager.at(2) == nullptr);
    (void)manager.add(nullptr);
    WOKE_CHECK(manager.count() == 2); // null registration refused

    // Category buckets are precomputed: the sidebar's badge is two int reads.
    WOKE_CHECK(manager.category_total(Category::Movement) == 1);
    WOKE_CHECK(manager.category_total(Category::Visual) == 1);
    WOKE_CHECK(manager.category_total(Category::Combat) == 0);
    WOKE_CHECK(manager.category_enabled(Category::Movement) == 0);

    // Setting metadata walked through the erased view.
    WOKE_CHECK(sprint.setting_count() == 1);
    WOKE_CHECK(zoom.setting_count() == 2);
    WOKE_CHECK_STR(zoom.settings()[0]->name(), "factor");
    WOKE_CHECK_STR(zoom.settings()[1]->name(), "curve");
    WOKE_CHECK(zoom.settings()[0]->kind() == woke::settings::Kind::Slider);

    // Toggle + fan-out.
    bool ticked = false;
    struct TickProbe {
        bool* flag;
    } probe{&ticked};
    (void)probe;

    WOKE_CHECK(zoom.enable());
    WOKE_CHECK(zoom.enabled());
    WOKE_CHECK(!zoom.enable());          // already on: no state change
    // A direct module->enable() bypasses the manager, so the buckets are refreshed explicitly -
    // the same mutate-then-report pattern the hook engine's deferred removal uses. The GUI and
    // config engine call notify_changed() after direct toggles; the manager's own paths
    // (handle_key, disable_all, set_category_enabled) refresh internally.
    manager.notify_changed();
    WOKE_CHECK(manager.enabled_count() == 1);
    WOKE_CHECK(manager.category_enabled(Category::Visual) == 1);

    WOKE_CHECK(zoom.disable());
    WOKE_CHECK(!zoom.enabled());
    manager.notify_changed();
    WOKE_CHECK(manager.category_enabled(Category::Visual) == 0);

    // Keybind dispatch: the Zoom module's default bind is 'C' (0x43).
    WOKE_CHECK(manager.handle_key(0x43, true));
    WOKE_CHECK(zoom.enabled());
    WOKE_CHECK(manager.handle_key(0x43, true)); // press again toggles back off
    WOKE_CHECK(!zoom.enabled());
    WOKE_CHECK(!manager.handle_key(0x43, false)); // key-up never toggles
    WOKE_CHECK(!manager.handle_key(0x5A, true)); // unbound key

    // Panic: disables everything, reports how many were live.
    WOKE_CHECK(zoom.enable());
    WOKE_CHECK(sprint.enable());
    WOKE_CHECK(manager.disable_all() == 2);
    WOKE_CHECK(manager.enabled_count() == 0);
    WOKE_CHECK(manager.disable_all() == 0); // nothing left to disable

    // Refused enable: a module whose on_enable returns false stays off.
    struct Refuser final : BaseModule {
        Refuser() noexcept
            : BaseModule("Refuser", "never enables", Category::Misc, 0) {}
        bool on_enable() noexcept override { return false; }
        void on_disable() noexcept override {}
    } refuser;
    (void)manager.add(&refuser);
    WOKE_CHECK(!refuser.enable());
    WOKE_CHECK(!refuser.enabled());
    WOKE_CHECK(manager.count() == 3);

    // Duplicate names are refused: the second registration cannot shadow the first.
    woke::modules::SprintState duplicate;
    (void)manager.add(&duplicate);
    WOKE_CHECK(manager.count() == 3);

    // ── Config persistence, including the reinjection boundary ──────────────────
    woke_test::section("config persistence");

    // A clean registry for the persistence act: exactly the modules a fresh injection registers
    // (the Refuser above was a local probe and does not exist in the "reinject").
    woke::modules::ModuleManager& registry = woke::modules::manager();
    registry.reset();
    (void)registry.add(&sprint);
    (void)registry.add(&zoom);

    MemoryStorage storage;
    woke::config::set_storage(&storage);

    // Change state: enable Sprint, set a zoom factor and curve.
    WOKE_CHECK(sprint.enable());
    zoom.settings()[0]->set_float(4.5f);
    zoom.settings()[1]->set_enum_index(2);
    WOKE_CHECK(sprint.enabled());
    WOKE_CHECK(util_nearly(zoom.settings()[0]->float_value(), 4.5f));
    WOKE_CHECK(zoom.settings()[1]->enum_index() == 2);

    // Dirty flags: only the touched settings are marked. (Enabling a module is not a setting
    // change; the enabled state is persisted, the per-setting dirty flags track settings.)
    WOKE_CHECK(!sprint.dirty());
    WOKE_CHECK(zoom.settings()[0]->dirty());
    WOKE_CHECK(zoom.settings()[1]->dirty());
    for (std::size_t index = 0; index < zoom.setting_count(); ++index) {
        WOKE_CHECK(zoom.settings()[index]->dirty());
    }
    zoom.clear_dirty();
    WOKE_CHECK(!zoom.dirty());

    // Save.
    WOKE_CHECK(woke::config::save("default"));
    WOKE_CHECK(storage.writes_ == 1);
    WOKE_CHECK(storage.exists(woke::config::path_for("default")));
    WOKE_CHECK_STR(woke::config::path_for("default").c_str(), "configs/default.json");

    const std::string saved = storage.files_[woke::config::path_for("default")];
    WOKE_CHECK(saved.find("\"Sprint State\"") != std::string::npos);
    WOKE_CHECK(saved.find("\"Zoom Amount\"") != std::string::npos);
    WOKE_CHECK(saved.find("4.5") != std::string::npos);

    // ── The reinjection boundary: tear everything down and rebuild ──────────────
    woke::modules::SprintState sprint2;
    woke::modules::ZoomAmount zoom2;
    fresh_registry(sprint2, zoom2);
    WOKE_CHECK(manager.count() == 2);
    WOKE_CHECK(!sprint2.enabled());                 // defaults back
    WOKE_CHECK(util_nearly(zoom2.settings()[0]->float_value(), 3.0f));
    WOKE_CHECK(zoom2.settings()[1]->enum_index() == 1);

    // Load: state comes back.
    const LoadReport loaded = woke::config::load("default");
    WOKE_CHECK(loaded.parsed);
    WOKE_CHECK(loaded.modules_matched == 2);
    // Applied = values that actually changed: sprint's enabled flag and bool, the factor, the
    // curve. (Zoom's enabled=false matches its default, so setting it is a no-op that does not
    // count - the "only real changes" rule.)
    WOKE_CHECK(loaded.settings_applied == 4);
    WOKE_CHECK(loaded.unknown_modules == 0);
    WOKE_CHECK(loaded.unknown_settings == 0);
    WOKE_CHECK(loaded.wrong_type == 0);

    WOKE_CHECK(sprint2.enabled());                  // toggles persisted across reinjection
    WOKE_CHECK(util_nearly(zoom2.settings()[0]->float_value(), 4.5f));
    WOKE_CHECK(zoom2.settings()[1]->enum_index() == 2);

    // A second save with unchanged state reproduces the same document (nlohmann's object dumps
    // sort keys, so the order is deterministic).
    WOKE_CHECK(woke::config::save("default"));
    WOKE_CHECK(storage.writes_ == 2);
    WOKE_CHECK(storage.files_[woke::config::path_for("default")] == saved);

    // ── Tolerance contract (§4.2) ───────────────────────────────────────────────
    woke_test::section("config tolerance");

    // Unknown module and unknown setting keys are counted, not fatal.
    const LoadReport unknown = woke::config::deserialize(R"({
        "Ghost Module": {"whatever": true},
        "Zoom Amount": {"factor": 5.0, "nope": 1}
    })");
    WOKE_CHECK(unknown.parsed);
    WOKE_CHECK(unknown.unknown_modules == 1);
    WOKE_CHECK(unknown.unknown_settings == 1);
    WOKE_CHECK(util_nearly(zoom2.settings()[0]->float_value(), 5.0f)); // the known part applied

    // Wrong-typed values are ignored, defaults keep standing.
    zoom2.settings()[0]->set_float(2.0f);
    const LoadReport bad = woke::config::deserialize(R"({
        "Zoom Amount": {"factor": "not a number", "curve": "quantum"}
    })");
    WOKE_CHECK(bad.parsed);
    WOKE_CHECK(bad.wrong_type == 2);
    WOKE_CHECK(bad.settings_applied == 0);
    WOKE_CHECK(util_nearly(zoom2.settings()[0]->float_value(), 2.0f));

    // Malformed JSON: parsed stays false, nothing applied, no crash.
    zoom2.settings()[0]->set_float(2.5f);
    const LoadReport broken = woke::config::deserialize("{not json");
    WOKE_CHECK(!broken.parsed);
    WOKE_CHECK(broken.settings_applied == 0);
    WOKE_CHECK(util_nearly(zoom2.settings()[0]->float_value(), 2.5f));

    // A non-object document is refused the same way.
    const LoadReport array_doc = woke::config::deserialize("[1, 2, 3]");
    WOKE_CHECK(!array_doc.parsed);

    // Missing file: first boot, defaults stay, nothing is an error.
    const LoadReport missing = woke::config::load("does-not-exist");
    WOKE_CHECK(!missing.parsed);
    WOKE_CHECK(missing.settings_applied == 0);

    // An empty registry serializes to an empty document, and saving that is a no-op failure.

    // The deferred-save mailbox: a request is coalesced into one write, serviced by whoever
    // calls service_saves() (the worker loop in production), and uses the requested name.
    woke_test::section("config deferred save");

    woke::modules::SprintState sprint3;
    woke::modules::ZoomAmount zoom3;
    woke::modules::ModuleManager& mailbox_registry = woke::modules::manager();
    mailbox_registry.reset();
    (void)mailbox_registry.add(&sprint3);
    (void)mailbox_registry.add(&zoom3);

    MemoryStorage mailbox_storage;
    woke::config::set_storage(&mailbox_storage);

    woke::config::request_save("default");
    woke::config::request_save("default"); // coalesced: still one pending write
    WOKE_CHECK(woke::config::service_saves());
    WOKE_CHECK(!woke::config::service_saves()); // drained: the second call is a no-op
    WOKE_CHECK(mailbox_storage.writes_ == 1);
    WOKE_CHECK(mailbox_storage.exists(woke::config::path_for("default")));

    // The saved document reflects the live registry (the request was "save current state").
    WOKE_CHECK(mailbox_storage.files_[woke::config::path_for("default")]
        .find("\"Sprint State\"") != std::string::npos);

    woke::config::set_storage(nullptr); // restore the real (file) store before the local store dies
    WOKE_CHECK(true);
}
