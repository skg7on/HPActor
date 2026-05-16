#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/messages.pb.h>
#include <scheduler_test_driver.hpp>

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace hpactor;

// Helper: poll until condition is true or timeout expires
template <typename Fn>
static bool
poll_until(Fn&& condition,
           int timeout_ms = 5000) { // NOLINT(cppcoreguidelines-missing-std-forward)
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}

// Actor that records received DownMsg notifications
class DownRecordingActor : public EventBasedActor {
  public:
    DownRecordingActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    int down_count() const {
        return down_count_;
    }
    ActorId last_down_actor() const {
        return last_down_actor_;
    }

    // Coroutine-based message processing: keep the actor alive by looping
    // over mailbox messages and dispatching through receive().
    sched::CoroutineTask act() override {
        while (true) {
            auto msg = co_await make_mailbox_awaiter();
            receive(msg);
        }
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == TypeTag::DownMsg) {
                auto pb = std::make_shared<::hpactor::DownMessage>();
                if (pb->ParseFromArray(msg.payload().data(),
                                       static_cast<int>(msg.payload().size()))) {
                    ++down_count_;
                    last_down_actor_ = ActorId(pb->actor_id());
                }
            }
        }};
    }

  private:
    int down_count_ = 0;
    ActorId last_down_actor_;
};

// =============================================================================
// Tests that require coroutine-based scheduling (on_exit is only called
// from the coroutine path in scheduler.cpp::execute_actor).
// =============================================================================
#if HPACTOR_SUPPORT_COROUTINES

// Actor that exits after processing one message.
// The coroutine path calls on_exit() when the coroutine completes.
class ShortLivedActor : public EventBasedActor {
  public:
    ShortLivedActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    sched::CoroutineTask act() override {
        // Process system messages (LinkMsg, MonitorMsg, etc.) in a loop.
        // Exit when a User message is received (the test trigger).
        while (true) {
            auto msg = co_await make_mailbox_awaiter();
            receive(msg);
            if (msg.type_id() == TypeTag::User) {
                set_exit_reason(errors::actor_down);
                co_return;
            }
        }
    }
};

// Test: A links to B. B exits. A receives DownMsg.
void test_link_to_down_notification() {
    Config config{.scheduler_threads = 1,
                  .max_queue_depth = 1024,
                  .use_coroutines = true,
                  .cli = {},
                  .mailbox = {},
                  .dead_letters = {},
                  .scheduler_start_paused = true,
                  .tracing = {}};
    ActorSystem system(config);
    hpactor::test::SchedulerTestDriver driver(system);

    auto a = system.spawn<DownRecordingActor>();
    auto b = system.spawn<ShortLivedActor>();

    // A links to B
    a.get()->link_to(b.address());

    // Verify A has B in linked_ list
    auto* ctx_a = static_cast<DownRecordingActor*>(a.get().get())->context();
    assert(ctx_a != nullptr);
    bool found = false;
    for (const auto& linked : ctx_a->linked_actors()) {
        if (linked == b.address()) {
            found = true;
            break;
        }
    }
    assert(found);

    // Deliver message to trigger B's coroutine (which will exit after one msg)
    system.deliver_local(b.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));

    auto* rec = static_cast<DownRecordingActor*>(a.get().get());
    bool received = driver.drain_until([rec, &b]() {
        return rec->down_count() >= 1 && rec->last_down_actor() == b.id();
    });
    assert(received);
}

// Test: monitor is one-way — A monitors B, B exits, A gets DownMsg
void test_monitor_down_notification() {
    Config config{.scheduler_threads = 1,
                  .max_queue_depth = 1024,
                  .use_coroutines = true,
                  .cli = {},
                  .mailbox = {},
                  .dead_letters = {},
                  .scheduler_start_paused = true,
                  .tracing = {}};
    ActorSystem system(config);
    hpactor::test::SchedulerTestDriver driver(system);

    auto a = system.spawn<DownRecordingActor>();
    auto b = system.spawn<ShortLivedActor>();

    a.get()->monitor(b.address());

    system.deliver_local(b.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));

    auto* rec = static_cast<DownRecordingActor*>(a.get().get());
    bool received = driver.drain_until([rec, &b]() {
        return rec->down_count() >= 1 && rec->last_down_actor() == b.id();
    });
    assert(received);
}

// Test: unlink_from removes the link, no DownMsg after unlink
void test_unlink_from_stops_notification() {
    Config config{.scheduler_threads = 1,
                  .max_queue_depth = 1024,
                  .use_coroutines = true,
                  .cli = {},
                  .mailbox = {},
                  .dead_letters = {},
                  .scheduler_start_paused = true,
                  .tracing = {}};
    ActorSystem system(config);
    hpactor::test::SchedulerTestDriver driver(system);

    auto a = system.spawn<DownRecordingActor>();
    auto b = system.spawn<ShortLivedActor>();

    a.get()->link_to(b.address());
    a.get()->unlink_from(b.address());

    // Verify link removed
    auto* ctx_a = static_cast<DownRecordingActor*>(a.get().get())->context();
    for (const auto& linked : ctx_a->linked_actors()) {
        assert(!(linked == b.address()));
    }

    system.deliver_local(b.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));

    driver.drain();
    auto* rec = static_cast<DownRecordingActor*>(a.get().get());
    assert(rec->down_count() == 0);
}

// Test: demonitor stops monitoring
void test_demonitor_stops_notification() {
    Config config{.scheduler_threads = 1,
                  .max_queue_depth = 1024,
                  .use_coroutines = true,
                  .cli = {},
                  .mailbox = {},
                  .dead_letters = {},
                  .scheduler_start_paused = true,
                  .tracing = {}};
    ActorSystem system(config);
    hpactor::test::SchedulerTestDriver driver(system);

    auto a = system.spawn<DownRecordingActor>();
    auto b = system.spawn<ShortLivedActor>();

    a.get()->monitor(b.address());
    a.get()->demonitor(b.address());

    system.deliver_local(b.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));

    driver.drain();
    auto* rec = static_cast<DownRecordingActor*>(a.get().get());
    assert(rec->down_count() == 0);
}

// Test: link to self is rejected
void test_link_to_dead_or_self() {
    Config config{.scheduler_threads = 1,
                  .max_queue_depth = 1024,
                  .use_coroutines = true,
                  .cli = {},
                  .mailbox = {},
                  .dead_letters = {},
                  .tracing = {}};
    ActorSystem system(config);

    auto a = system.spawn<DownRecordingActor>();

    // link-to-self should be rejected
    a.get()->link_to(a.get()->address());

    auto* ctx_a = static_cast<DownRecordingActor*>(a.get().get())->context();
    int self_count = 0;
    for (const auto& linked : ctx_a->linked_actors()) {
        if (linked == a.get()->address())
            ++self_count;
    }
    assert(self_count == 0);
}

#else // !HPACTOR_SUPPORT_COROUTINES

// Stub tests when coroutine support is not compiled in.
// Death propagation (on_exit) requires the coroutine scheduler path.
void test_link_to_down_notification() {
    std::cout << "SKIP: coroutines not available" << std::endl;
}
void test_monitor_down_notification() {
    std::cout << "SKIP: coroutines not available" << std::endl;
}
void test_unlink_from_stops_notification() {
    std::cout << "SKIP: coroutines not available" << std::endl;
}
void test_demonitor_stops_notification() {
    std::cout << "SKIP: coroutines not available" << std::endl;
}
void test_link_to_dead_or_self() {
    Config config{.scheduler_threads = 1,
                  .max_queue_depth = 1024,
                  .cli = {},
                  .mailbox = {},
                  .dead_letters = {},
                  .tracing = {}};
    ActorSystem system(config);
    auto a = system.spawn<DownRecordingActor>();
    a.get()->link_to(a.get()->address());
    auto* ctx_a = static_cast<DownRecordingActor*>(a.get().get())->context();
    int self_count = 0;
    for (const auto& linked : ctx_a->linked_actors()) {
        if (linked == a.get()->address())
            ++self_count;
    }
    assert(self_count == 0);
}

#endif // HPACTOR_SUPPORT_COROUTINES

// =============================================================================
// Tests that work regardless of coroutine support (protocol-only)
// =============================================================================

// Test: link_to is idempotent
void test_link_to_idempotent() {
    Config config{.scheduler_threads = 1,
                  .max_queue_depth = 1024,
                  .cli = {},
                  .mailbox = {},
                  .dead_letters = {},
                  .tracing = {}};
    ActorSystem system(config);

    auto a = system.spawn<DownRecordingActor>();
    auto b = system.spawn<DownRecordingActor>();

    a.get()->link_to(b.address());
    a.get()->link_to(b.address()); // duplicate

    // Should only have one entry
    auto* ctx_a = static_cast<DownRecordingActor*>(a.get().get())->context();
    int count = 0;
    for (const auto& linked : ctx_a->linked_actors()) {
        if (linked == b.address())
            ++count;
    }
    assert(count == 1);
}

// Test: link_to sends LinkMsg, which adds bidirectional entry on receiver
void test_link_to_sends_link_msg() {
    Config config{.scheduler_threads = 1,
                  .max_queue_depth = 1024,
                  .cli = {},
                  .mailbox = {},
                  .dead_letters = {},
                  .scheduler_start_paused = true,
                  .tracing = {}};
    ActorSystem system(config);
    hpactor::test::SchedulerTestDriver driver(system);

    auto a = system.spawn<DownRecordingActor>();
    auto b = system.spawn<DownRecordingActor>();

    a.get()->link_to(b.address());

    auto* ctx_b = static_cast<DownRecordingActor*>(b.get().get())->context();
    bool delivered = driver.drain_until([ctx_b, &a]() {
        for (const auto& linked : ctx_b->linked_actors()) {
            if (linked == a.get()->address())
                return true;
        }
        return false;
    });
    assert(delivered);
}

int main() {
    test_link_to_down_notification();
    std::cout << "PASS: test_link_to_down_notification" << std::endl;

    test_monitor_down_notification();
    std::cout << "PASS: test_monitor_down_notification" << std::endl;

    test_unlink_from_stops_notification();
    std::cout << "PASS: test_unlink_from_stops_notification" << std::endl;

    test_demonitor_stops_notification();
    std::cout << "PASS: test_demonitor_stops_notification" << std::endl;

    test_link_to_idempotent();
    std::cout << "PASS: test_link_to_idempotent" << std::endl;

    test_link_to_dead_or_self();
    std::cout << "PASS: test_link_to_dead_or_self" << std::endl;

    test_link_to_sends_link_msg();
    std::cout << "PASS: test_link_to_sends_link_msg" << std::endl;

    return 0;
}
