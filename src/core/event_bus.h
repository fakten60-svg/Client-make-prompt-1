#pragma once

// Type-safe, fixed-slot publish/subscribe (blueprint §4.1).
//
//   woke::events::bus().subscribe<KeyEvent, &on_key>(module);
//   woke::events::bus().post(key);
//
// Three properties matter and all three are structural rather than aspirational:
//
//   * Zero allocation. Each event type owns a pre-sized slot array; subscribing fills the
//     next free slot and posting is a linear sweep of at most 16 handlers. There is no
//     std::function, no vector growth and no heap traffic after boot.
//   * O(1) unsubscribe, which is what makes module disable cheap: the slot's function
//     pointer is simply nulled and the sweep skips it.
//   * No locks. Every post happens on the game thread (§6.1), so there is nothing to
//     synchronize. Posting from another thread is a design error, not a supported case.
//
// This header is portable: no windows.h, no logger, no JNI. That is deliberate, because the
// bus sits underneath every module and is therefore covered by the host test suite on Linux
// (dispatch order, slot reuse, unsubscribe during dispatch, overflow) instead of only by an
// in-game session.

#include <array>
#include <cstddef>
#include <cstdint>

namespace woke::events {

// Runtime identity of an event type. Assigned on first use, one per type, starting at 0.
using TypeId = std::size_t;

// Type-erased handler. A trampoline (below) adapts each compile-time handler to this shape,
// so the bus itself never needs a type tag, a dynamic cast or a std::function.
using Trampoline = void (*)(void* context, void* payload);

struct Handler {
    Trampoline call = nullptr;
    void* context = nullptr;
};

// Handle returned by subscribe(). Deliberately trivially copyable: modules can store it in a
// fixed array and hand it back to unsubscribe() later.
//
// `generation` makes a stale handle harmless. Slots are recycled, so without it a handle kept
// from a previous enable would unsubscribe whichever subscriber later inherited its slot -
// a bug that only shows up as "some module randomly stopped receiving input".
struct Subscription {
    TypeId type = 0;
    std::size_t slot = 0;
    std::uint32_t generation = 0;
    bool active = false;

    [[nodiscard]] bool valid() const noexcept { return active; }
    void reset() noexcept { active = false; }
};

namespace detail {

// Defined in event_bus.cpp so the counter lives in exactly one translation unit.
[[nodiscard]] TypeId acquire_type_id() noexcept;

template <typename Event, void (*HandlerFn)(Event&)>
void trampoline_plain(void* /*context*/, void* payload) noexcept {
    HandlerFn(*static_cast<Event*>(payload));
}

template <typename Event, typename Context, void (*HandlerFn)(Context&, Event&)>
void trampoline_with_context(void* context, void* payload) noexcept {
    HandlerFn(*static_cast<Context*>(context), *static_cast<Event*>(payload));
}

} // namespace detail

// One stable id per event type, assigned the first time it is used. Every caller is a
// boot-time subscriber, so the function-local static's guard is never contended.
template <typename Event>
[[nodiscard]] TypeId type_id() noexcept {
    static const TypeId id = detail::acquire_type_id();
    return id;
}

class Bus {
public:
    // 16 event types x 16 handlers is far beyond what the module catalog needs (8 event
    // types, at most a handful of subscribers each); exceeding it is reported rather than
    // silently dropping a subscription.
    static constexpr std::size_t kMaxChannels = 16;
    static constexpr std::size_t kSlotsPerChannel = 16;

    // Free-function subscriber.
    template <typename Event, void (*HandlerFn)(Event&)>
    [[nodiscard]] Subscription subscribe() noexcept {
        return add(type_id<Event>(), &detail::trampoline_plain<Event, HandlerFn>, nullptr);
    }

    // Member-style subscriber: the context is whatever the handler wants it to be, and no
    // type erasure happens at the call site.
    template <typename Event, typename Context, void (*HandlerFn)(Context&, Event&)>
    [[nodiscard]] Subscription subscribe(Context& context) noexcept {
        return add(type_id<Event>(), &detail::trampoline_with_context<Event, Context, HandlerFn>,
            &context);
    }

    void unsubscribe(Subscription subscription) noexcept;

    template <typename Event>
    void post(Event& event) noexcept {
        const TypeId id = type_id<Event>();
        if (id >= kMaxChannels) {
            return;
        }

        Channel& channel = channels_[id];
        // Walk by index and re-read each slot: a handler that unsubscribes (or subscribes)
        // during dispatch only touches its own slot, so no snapshot and no invalidation.
        for (std::size_t slot = 0; slot < kSlotsPerChannel; ++slot) {
            const Handler handler = channel.slots[slot];
            if (handler.call != nullptr) {
                handler.call(handler.context, &event);
            }
        }
    }

    template <typename Event>
    [[nodiscard]] std::size_t handler_count() const noexcept {
        const TypeId id = type_id<Event>();
        return id >= kMaxChannels ? 0 : channels_[id].count;
    }

    [[nodiscard]] std::size_t active_channel_count() const noexcept { return channels_used_; }
    [[nodiscard]] std::size_t total_handler_count() const noexcept;
    [[nodiscard]] std::size_t rejected_subscription_count() const noexcept { return rejected_; }

    // Drops every handler. Used by boot so a re-injection cannot inherit stale subscribers.
    void reset() noexcept;

private:
    struct Channel {
        std::array<Handler, kSlotsPerChannel> slots{};
        std::array<std::uint32_t, kSlotsPerChannel> generations{};
        std::size_t count = 0;
    };

    [[nodiscard]] Subscription add(TypeId type, Trampoline call, void* context) noexcept;

    std::array<Channel, kMaxChannels> channels_{};
    std::size_t channels_used_ = 0;
    std::size_t rejected_ = 0;
};

// The process-wide bus. One instance, owned by event_bus.cpp.
[[nodiscard]] Bus& bus() noexcept;

} // namespace woke::events
