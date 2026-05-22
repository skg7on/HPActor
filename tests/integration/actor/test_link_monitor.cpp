// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/messages.pb.h>
#include <scheduler_test_driver.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace hpactor;

// Helper: poll until condition is true or timeout expires
template <typename Fn>
static bool poll_until(Fn&& condition, int timeout_ms = 5000) {
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

    // Coroutine-based message processing
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

// Coroutine-based tests (use scheduler + TestDriver)
#if HPACTOR_SUPPORT_COROUTINES

// Actor that exits after processing one message.
class ShortLivedActor : public EventBasedActor {
  public:
    ShortLivedActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    sched::CoroutineTask act() override {
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

class LinkMonitorCoroutineTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config config{.scheduler_threads = 1,
                      .max_queue_depth = 1024,
                      .use_coroutines = true,
                      .cli = {},
                      .mailbox = {},
                      .dead_letters = {},
                      .scheduler_start_paused = true,
                      .tracing = {}};
        system_ = std::make_unique<ActorSystem>(config);
        driver_ = std::make_unique<hpactor::test::SchedulerTestDriver>(*system_);
    }
    void TearDown() override {
        if (system_) {
            ShutdownOptions opts;
            opts.ingress_timeout = std::chrono::milliseconds(10);
            opts.actor_drain_timeout = std::chrono::milliseconds(100);
            opts.cluster_leave_timeout = std::chrono::milliseconds(10);
            system_->shutdown(opts);
        }
    }
    std::unique_ptr<ActorSystem> system_;
    std::unique_ptr<hpactor::test::SchedulerTestDriver> driver_;
};

TEST_F(LinkMonitorCoroutineTest, LinkToDownNotification) {
    auto a = system_->spawn<DownRecordingActor>();
    auto b = system_->spawn<ShortLivedActor>();

    a.get()->link_to(b.address());

    // Verify A has B in linked_ list
    auto* ctx_a = static_cast<DownRecordingActor*>(a.get().get())->context();
    ASSERT_NE(ctx_a, nullptr);
    bool found = false;
    for (const auto& linked : ctx_a->linked_actors()) {
        if (linked == b.address()) {
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);

    // Deliver message to trigger B's coroutine (which will exit after one msg)
    system_->deliver_local(b.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));

    auto* rec = static_cast<DownRecordingActor*>(a.get().get());
    bool received = driver_->drain_until([rec, &b]() {
        return rec->down_count() >= 1 && rec->last_down_actor() == b.id();
    });
    EXPECT_TRUE(received);
}

TEST_F(LinkMonitorCoroutineTest, MonitorDownNotification) {
    auto a = system_->spawn<DownRecordingActor>();
    auto b = system_->spawn<ShortLivedActor>();

    a.get()->monitor(b.address());

    system_->deliver_local(b.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));

    auto* rec = static_cast<DownRecordingActor*>(a.get().get());
    bool received = driver_->drain_until([rec, &b]() {
        return rec->down_count() >= 1 && rec->last_down_actor() == b.id();
    });
    EXPECT_TRUE(received);
}

TEST_F(LinkMonitorCoroutineTest, UnlinkFromStopsNotification) {
    auto a = system_->spawn<DownRecordingActor>();
    auto b = system_->spawn<ShortLivedActor>();

    a.get()->link_to(b.address());
    a.get()->unlink_from(b.address());

    // Verify link removed
    auto* ctx_a = static_cast<DownRecordingActor*>(a.get().get())->context();
    for (const auto& linked : ctx_a->linked_actors()) {
        ASSERT_FALSE(linked == b.address());
    }

    system_->deliver_local(b.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));

    driver_->drain();
    auto* rec = static_cast<DownRecordingActor*>(a.get().get());
    EXPECT_EQ(rec->down_count(), 0);
}

TEST_F(LinkMonitorCoroutineTest, DemonitorStopsNotification) {
    auto a = system_->spawn<DownRecordingActor>();
    auto b = system_->spawn<ShortLivedActor>();

    a.get()->monitor(b.address());
    a.get()->demonitor(b.address());

    system_->deliver_local(b.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));

    driver_->drain();
    auto* rec = static_cast<DownRecordingActor*>(a.get().get());
    EXPECT_EQ(rec->down_count(), 0);
}

TEST_F(LinkMonitorCoroutineTest, LinkToDeadOrSelf) {
    auto a = system_->spawn<DownRecordingActor>();

    // link-to-self should be rejected
    a.get()->link_to(a.get()->address());

    auto* ctx_a = static_cast<DownRecordingActor*>(a.get().get())->context();
    int self_count = 0;
    for (const auto& linked : ctx_a->linked_actors()) {
        if (linked == a.get()->address())
            ++self_count;
    }
    EXPECT_EQ(self_count, 0);
}

#else // !HPACTOR_SUPPORT_COROUTINES

// When coroutines are not available, skip the coroutine-dependent tests
// but still run the protocol-only tests.

class LinkMonitorNoCoroutineTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config config{.scheduler_threads = 1,
                      .max_queue_depth = 1024,
                      .cli = {},
                      .mailbox = {},
                      .dead_letters = {},
                      .tracing = {}};
        system_ = std::make_unique<ActorSystem>(config);
    }
    void TearDown() override {
        if (system_) {
            ShutdownOptions opts;
            opts.ingress_timeout = std::chrono::milliseconds(10);
            opts.actor_drain_timeout = std::chrono::milliseconds(10);
            opts.cluster_leave_timeout = std::chrono::milliseconds(10);
            system_->shutdown(opts);
        }
    }
    std::unique_ptr<ActorSystem> system_;
};

TEST_F(LinkMonitorNoCoroutineTest, LinkToDownNotification) {
    GTEST_SKIP() << "Coroutines not available";
}
TEST_F(LinkMonitorNoCoroutineTest, MonitorDownNotification) {
    GTEST_SKIP() << "Coroutines not available";
}
TEST_F(LinkMonitorNoCoroutineTest, UnlinkFromStopsNotification) {
    GTEST_SKIP() << "Coroutines not available";
}
TEST_F(LinkMonitorNoCoroutineTest, DemonitorStopsNotification) {
    GTEST_SKIP() << "Coroutines not available";
}
TEST_F(LinkMonitorNoCoroutineTest, LinkToDeadOrSelf) {
    GTEST_SKIP() << "Coroutines not available";
}

#endif // HPACTOR_SUPPORT_COROUTINES

// Tests that work regardless of coroutine support (protocol-only)

class LinkMonitorProtocolTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config config{.scheduler_threads = 1,
                      .max_queue_depth = 1024,
                      .cli = {},
                      .mailbox = {},
                      .dead_letters = {},
                      .tracing = {}};
        system_ = std::make_unique<ActorSystem>(config);
    }
    void TearDown() override {
        if (system_) {
            ShutdownOptions opts;
            opts.ingress_timeout = std::chrono::milliseconds(10);
            opts.actor_drain_timeout = std::chrono::milliseconds(10);
            opts.cluster_leave_timeout = std::chrono::milliseconds(10);
            system_->shutdown(opts);
        }
    }
    std::unique_ptr<ActorSystem> system_;
};

TEST_F(LinkMonitorProtocolTest, LinkToIdempotent) {
    auto a = system_->spawn<DownRecordingActor>();
    auto b = system_->spawn<DownRecordingActor>();

    a.get()->link_to(b.address());
    a.get()->link_to(b.address()); // duplicate

    // Should only have one entry
    auto* ctx_a = static_cast<DownRecordingActor*>(a.get().get())->context();
    int count = 0;
    for (const auto& linked : ctx_a->linked_actors()) {
        if (linked == b.address())
            ++count;
    }
    EXPECT_EQ(count, 1);
}

TEST_F(LinkMonitorProtocolTest, LinkToSendsLinkMsg) {
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
    EXPECT_TRUE(delivered);
}

#if HPACTOR_SUPPORT_COROUTINES
// When coroutines are available, also test link-to-self in the coroutine path
#else
// When coroutines are not available, test link-to-self in non-coroutine path
TEST_F(LinkMonitorProtocolTest, LinkToDeadOrSelfNonCoroutine) {
    auto a = system_->spawn<DownRecordingActor>();
    a.get()->link_to(a.get()->address());
    auto* ctx_a = static_cast<DownRecordingActor*>(a.get().get())->context();
    int self_count = 0;
    for (const auto& linked : ctx_a->linked_actors()) {
        if (linked == a.get()->address())
            ++self_count;
    }
    EXPECT_EQ(self_count, 0);
}
#endif
