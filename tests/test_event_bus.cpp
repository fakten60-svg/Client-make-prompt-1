#include "test_harness.h"

#include "core/event_bus.h"
#include "core/events.h"

// The event bus is the substrate every module hangs off, and it is portable by design, so
// its contract is asserted here rather than only in a game session: dispatch order, per-type
// isolation, O(1) unsubscribe, slot reuse, mutation during dispatch, and the overflow policy.

namespace {

using woke::events::Bus;
using woke::events::KeyEvent;
using woke::events::Subscription;
using woke::events::TickEvent;

// Handler state. Globals because a handler is a plain function pointer with no capture.
struct Journal {
    int first_calls = 0;
    int second_calls = 0;
    int key_calls = 0;
    int order[4] = {0, 0, 0, 0};
    int order_size = 0;
    std::uint64_t last_tick = 0;
    bool saw_consumed_flag = false;
};

Journal g_journal{};
Bus* g_bus = nullptr;
Subscription g_second_subscription{};

void record(int tag) {
    if (g_journal.order_size < 4) {
        g_journal.order[g_journal.order_size] = tag;
        ++g_journal.order_size;
    }
}

void on_tick_first(TickEvent& tick) {
    ++g_journal.first_calls;
    g_journal.last_tick = tick.tick_index;
    record(1);
}

void on_tick_second(TickEvent& /*tick*/) {
    ++g_journal.second_calls;
    record(2);
    // Unsubscribing from inside a dispatch must not disturb the walk.
    g_bus->unsubscribe(g_second_subscription);
}

void on_key(KeyEvent& key) {
    ++g_journal.key_calls;
    key.consumed = true;
}

void on_key_observer(KeyEvent& key) {
    g_journal.saw_consumed_flag = key.consumed;
    record(4);
}

int g_handler_calls = 0;

// Overflow fixture: 17 distinct handlers for one event type, which is one more than a
// channel can hold.
template <int Index>
void on_overflow(woke::events::FrameEvent& /*frame*/) {
    ++g_handler_calls;
}

template <std::size_t... Index>
std::size_t subscribe_overflow_handlers(Bus& target, std::index_sequence<Index...>) {
    const bool accepted[] = {
        target.subscribe<woke::events::FrameEvent, &on_overflow<static_cast<int>(Index)>>().valid()...};
    std::size_t count = 0;
    for (const bool result : accepted) {
        if (result) {
            ++count;
        }
    }
    return count;
}

} // namespace

void test_event_bus() {
    woke_test::section("event bus");

    Bus& bus = woke::events::bus();
    g_bus = &bus;
    bus.reset();
    g_journal = Journal{};

    // ── Type identity ────────────────────────────────────────────────────────────────
    WOKE_CHECK(woke::events::type_id<TickEvent>() == woke::events::type_id<TickEvent>());
    WOKE_CHECK(woke::events::type_id<TickEvent>() != woke::events::type_id<KeyEvent>());
    WOKE_CHECK(woke::events::type_id<TickEvent>() < Bus::kMaxChannels);

    // ── Posting with no subscribers is a no-op ───────────────────────────────────────
    {
        TickEvent tick;
        bus.post(tick);
        WOKE_CHECK(bus.total_handler_count() == 0);
    }

    // ── Dispatch order, payload delivery, isolation between types ────────────────────
    {
        const Subscription first = bus.subscribe<TickEvent, &on_tick_first>();
        const Subscription second = bus.subscribe<TickEvent, &on_tick_second>();
        g_second_subscription = second;
        const Subscription key = bus.subscribe<KeyEvent, &on_key>();

        WOKE_CHECK(first.valid());
        WOKE_CHECK(second.valid());
        WOKE_CHECK(key.valid());
        WOKE_CHECK(first.slot != second.slot);
        WOKE_CHECK(bus.handler_count<TickEvent>() == 2);
        WOKE_CHECK(bus.handler_count<KeyEvent>() == 1);
        WOKE_CHECK(bus.handler_count<woke::events::MouseEvent>() == 0);
        WOKE_CHECK(bus.total_handler_count() == 3);

        TickEvent tick;
        tick.tick_index = 42;
        bus.post(tick);

        WOKE_CHECK(g_journal.first_calls == 1);
        WOKE_CHECK(g_journal.second_calls == 1);
        WOKE_CHECK(g_journal.last_tick == 42);
        WOKE_CHECK(g_journal.order_size == 2);
        WOKE_CHECK(g_journal.order[0] == 1); // subscription order is dispatch order
        WOKE_CHECK(g_journal.order[1] == 2);
        WOKE_CHECK(g_journal.key_calls == 0); // a TickEvent is not delivered to KeyEvent slots

        // The second handler unsubscribed itself while the sweep was running; the sweep is
        // still valid and the next post reaches only the survivor.
        WOKE_CHECK(bus.handler_count<TickEvent>() == 1);
        g_journal.second_calls = 0;
        bus.post(tick);
        WOKE_CHECK(g_journal.first_calls == 2);
        WOKE_CHECK(g_journal.second_calls == 0);

        // A handler that saw an earlier handler's mutation sees the mutation.
        [[maybe_unused]] const Subscription observer = bus.subscribe<KeyEvent, &on_key_observer>();
        KeyEvent key_event;
        bus.post(key_event);
        WOKE_CHECK(g_journal.key_calls == 1);
        WOKE_CHECK(key_event.consumed);
        WOKE_CHECK(g_journal.saw_consumed_flag);

        bus.unsubscribe(key);
        bus.unsubscribe(first);
        WOKE_CHECK(bus.handler_count<KeyEvent>() == 1);
        WOKE_CHECK(bus.handler_count<TickEvent>() == 0);
    }

    // ── Unsubscribe is idempotent, and a freed slot is reused ────────────────────────
    {
        bus.reset();
        const Subscription subscription = bus.subscribe<TickEvent, &on_tick_first>();
        WOKE_CHECK(subscription.valid());
        WOKE_CHECK(subscription.slot == 0);

        bus.unsubscribe(subscription);
        bus.unsubscribe(subscription); // second call must be harmless
        WOKE_CHECK(bus.handler_count<TickEvent>() == 0);

        const Subscription replacement = bus.subscribe<TickEvent, &on_tick_first>();
        WOKE_CHECK(replacement.valid());
        WOKE_CHECK(replacement.slot == 0); // first free slot, no permanent slot burn
        bus.unsubscribe(replacement);
    }

    // ── A stale handle cannot evict the subscriber that took its slot ────────────────
    {
        bus.reset();
        const Subscription stale = bus.subscribe<TickEvent, &on_tick_first>();
        bus.unsubscribe(stale);
        const Subscription fresh = bus.subscribe<TickEvent, &on_tick_first>();
        WOKE_CHECK(fresh.slot == stale.slot);

        // The old handle is already marked inactive, so this is ignored instead of removing
        // the live subscriber that inherited the slot.
        bus.unsubscribe(stale);
        WOKE_CHECK(bus.handler_count<TickEvent>() == 1);
        bus.reset();
    }

    // ── Overflow: the 17th handler for one event type is refused, not dropped silently ──
    {
        bus.reset();
        g_handler_calls = 0;
        const std::size_t accepted =
            subscribe_overflow_handlers(bus, std::make_index_sequence<Bus::kSlotsPerChannel + 1>{});
        WOKE_CHECK(accepted == Bus::kSlotsPerChannel);
        WOKE_CHECK(bus.handler_count<woke::events::FrameEvent>() == Bus::kSlotsPerChannel);
        WOKE_CHECK(bus.rejected_subscription_count() == 1);

        woke::events::FrameEvent frame;
        bus.post(frame);
        WOKE_CHECK(g_handler_calls == static_cast<int>(Bus::kSlotsPerChannel));

        // Refusing a subscriber must not have poisoned the channel: a reset frees it again.
        bus.reset();
        WOKE_CHECK(bus.handler_count<woke::events::FrameEvent>() == 0);
        WOKE_CHECK(bus.total_handler_count() == 0);
        const std::size_t accepted_again =
            subscribe_overflow_handlers(bus, std::make_index_sequence<Bus::kSlotsPerChannel>{});
        WOKE_CHECK(accepted_again == Bus::kSlotsPerChannel);
        bus.reset();
    }

    // ── Boot-time reset leaves a clean bus (re-injection safety) ─────────────────────
    {
        const Subscription tick = bus.subscribe<TickEvent, &on_tick_first>();
        const Subscription key = bus.subscribe<KeyEvent, &on_key>();
        WOKE_CHECK(tick.valid());
        WOKE_CHECK(key.valid());
        WOKE_CHECK(bus.active_channel_count() == 2);

        bus.reset();
        WOKE_CHECK(bus.active_channel_count() == 0);
        WOKE_CHECK(bus.total_handler_count() == 0);
        WOKE_CHECK(bus.rejected_subscription_count() == 0);
        WOKE_CHECK(bus.handler_count<TickEvent>() == 0);
    }

    g_bus = nullptr;
}
