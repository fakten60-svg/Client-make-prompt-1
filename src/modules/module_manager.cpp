#include "modules/module_manager.h"

#include <algorithm>

namespace woke::modules {

bool ModuleManager::add(BaseModule* module) noexcept {
    if (module == nullptr || count_ >= kMaxModules) {
        return false;
    }
    // One module per name: a duplicate would make config lookups and the GUI ambiguous, so the
    // second registration is refused rather than silently shadowed.
    if (find(module->name()) != nullptr) {
        return false;
    }
    modules_[count_++] = module;
    recompute_buckets();
    return true;
}

void ModuleManager::reset() noexcept {
    modules_.fill(nullptr);
    count_ = 0;
    totals_.fill(0);
    enabled_.fill(0);
}

BaseModule* ModuleManager::find(std::string_view name) const noexcept {
    for (std::size_t index = 0; index < count_; ++index) {
        if (name == modules_[index]->name()) {
            return modules_[index];
        }
    }
    return nullptr;
}

BaseModule* ModuleManager::at(std::size_t index) const noexcept {
    return index < count_ ? modules_[index] : nullptr;
}

std::size_t ModuleManager::category_total(Category category) const noexcept {
    const auto index = static_cast<std::size_t>(category);
    return index < kCategoryCount ? totals_[index] : 0;
}

std::size_t ModuleManager::category_enabled(Category category) const noexcept {
    const auto index = static_cast<std::size_t>(category);
    return index < kCategoryCount ? enabled_[index] : 0;
}

std::size_t ModuleManager::set_category_enabled(Category category, bool enabled) noexcept {
    std::size_t changed = 0;
    for (std::size_t index = 0; index < count_; ++index) {
        BaseModule* module = modules_[index];
        if (module->category() == category && module->set_enabled(enabled)) {
            ++changed;
        }
    }
    if (changed > 0) {
        recompute_buckets();
    }
    return changed;
}

std::size_t ModuleManager::disable_all() noexcept {
    std::size_t live = 0;
    for (std::size_t index = 0; index < count_; ++index) {
        if (modules_[index]->disable()) {
            ++live;
        }
    }
    if (live > 0) {
        recompute_buckets();
    }
    return live;
}

// NOTE: enable()/disable() on a BaseModule does not reach back into the manager (the module does
// not know which manager owns it), so a direct module->enable() through the registry leaves the
// enabled_ bucket stale. The GUI and the config engine therefore call notify_changed() after any
// direct toggle - the same pattern the deferred-removal hook engine uses: mutate, then report.

// Every module sees both edges and decides for itself: the default hook press-toggles, a hold
// module tracks the key, and an action module (Panic) fires once and stays off. The manager only
// owns the two facts it can own without knowing any module's policy - whether the key was claimed,
// and whether the enabled buckets need recomputing. It compares the enabled flag around each call
// rather than assuming a toggle, because an action module changes *other* modules' state.
bool ModuleManager::handle_key(int virtual_key, bool down) noexcept {
    if (virtual_key == 0) {
        return false;
    }
    bool consumed = false;
    bool changed = false;
    for (std::size_t index = 0; index < count_; ++index) {
        BaseModule* module = modules_[index];
        const bool was_enabled = module->enabled();
        if (module->on_key(virtual_key, down)) {
            consumed = true;
        }
        if (module->enabled() != was_enabled) {
            changed = true;
        }
    }
    if (changed) {
        recompute_buckets();
    }
    return consumed;
}

bool ModuleManager::has_toggle_bind(int virtual_key) const noexcept {
    if (virtual_key == 0) {
        return false;
    }
    for (std::size_t index = 0; index < count_; ++index) {
        const BaseModule* module = modules_[index];
        if (!module->is_action() && module->bind() == virtual_key) {
            return true;
        }
    }
    return false;
}

void ModuleManager::on_tick(float delta_seconds) noexcept {
    for (std::size_t index = 0; index < count_; ++index) {
        BaseModule* module = modules_[index];
        if (module->enabled()) {
            module->on_tick(delta_seconds);
        }
    }
}

void ModuleManager::on_render(float delta_seconds) noexcept {
    for (std::size_t index = 0; index < count_; ++index) {
        BaseModule* module = modules_[index];
        if (module->enabled()) {
            module->on_render(delta_seconds);
        }
    }
}

std::size_t ModuleManager::enabled_count() const noexcept {
    std::size_t total = 0;
    for (const std::size_t bucket : enabled_) {
        total += bucket;
    }
    return total;
}

void ModuleManager::notify_changed() noexcept {
    recompute_buckets();
}

void ModuleManager::recompute_buckets() noexcept {
    totals_.fill(0);
    enabled_.fill(0);
    for (std::size_t index = 0; index < count_; ++index) {
        const BaseModule* module = modules_[index];
        const auto bucket = static_cast<std::size_t>(module->category());
        if (bucket < kCategoryCount) {
            ++totals_[bucket];
            if (module->enabled()) {
                ++enabled_[bucket];
            }
        }
    }
}

ModuleManager& manager() noexcept {
    static ModuleManager instance;
    return instance;
}

} // namespace woke::modules
