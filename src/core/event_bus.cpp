#include "core/event_bus.h"

// Portable on purpose: no windows.h, no logger, no JNI, so the bus is linked into the host
// test target and its dispatch rules are asserted on Linux in seconds.

namespace woke::events {
namespace {

TypeId g_next_type_id = 0;

// Starts at 1 so a zero-initialised slot can never match a real handle. Wraps after four
// billion subscriptions, which is fine: a collision needs an exact (type, slot, generation)
// match and a handle surviving that long.
std::uint32_t g_next_generation = 1;

} // namespace

namespace detail {

TypeId acquire_type_id() noexcept {
    return g_next_type_id++;
}

} // namespace detail

Subscription Bus::add(TypeId type, Trampoline call, void* context) noexcept {
    if (type >= kMaxChannels) {
        // More event types than the fixed table can hold. Reported as a count instead of a
        // log line so this file keeps no dependency on the logger.
        ++rejected_;
        return Subscription{};
    }

    Channel& channel = channels_[type];
    for (std::size_t slot = 0; slot < kSlotsPerChannel; ++slot) {
        if (channel.slots[slot].call == nullptr) {
            channel.slots[slot] = Handler{call, context};
            channel.generations[slot] = g_next_generation++;
            ++channel.count;
            if (channel.count == 1 && type >= channels_used_) {
                channels_used_ = type + 1;
            }
            return Subscription{type, slot, channel.generations[slot], true};
        }
    }

    ++rejected_;
    return Subscription{};
}

void Bus::unsubscribe(Subscription subscription) noexcept {
    if (!subscription.active || subscription.type >= kMaxChannels
        || subscription.slot >= kSlotsPerChannel) {
        return;
    }

    Channel& channel = channels_[subscription.type];
    if (channel.slots[subscription.slot].call == nullptr) {
        return; // already unsubscribed
    }
    if (channel.generations[subscription.slot] != subscription.generation) {
        return; // stale handle: the slot now belongs to a later subscriber
    }

    channel.slots[subscription.slot] = Handler{};
    --channel.count;
}

std::size_t Bus::total_handler_count() const noexcept {
    std::size_t total = 0;
    for (const Channel& channel : channels_) {
        total += channel.count;
    }
    return total;
}

void Bus::reset() noexcept {
    for (Channel& channel : channels_) {
        channel.slots = {};
        channel.generations = {};
        channel.count = 0;
    }
    channels_used_ = 0;
    rejected_ = 0;
}

Bus& bus() noexcept {
    static Bus instance;
    return instance;
}

} // namespace woke::events
