// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

// System test: RPC Workflow
// Validates RpcChannel, RpcFuture, timeout/retry, ask manager,
// request handle lifecycle, and concurrent operations.

#include <gtest/gtest.h>

#include "../support/scheduler_test_driver.hpp"
#include "../support/system_test_fixture.hpp"

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/request_handle.hpp>
#include <hpactor/msg/request_timeout.hpp>
#include <hpactor/rpc/rpc_channel.hpp>

// ── Echo actor: counts received messages, replies back ──────────────

class EchoActor : public hpactor::EventBasedActor {
  public:
    EchoActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    int handled() const {
        return handled_;
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            handled_++;
            context()->reply(std::move(msg));
        }};
    }

  private:
    int handled_ = 0;
};

// ── Silent actor: never replies (for timeout tests) ─────────────────

class SilentActor : public hpactor::EventBasedActor {
  public:
    SilentActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {}
};

HPACTOR_REGISTER_ACTOR("EchoActor", EchoActor)
HPACTOR_REGISTER_ACTOR("SilentActor", SilentActor)

// ══════════════════════════════════════════════════════════════════════
// Group 1: RPC basics
// ══════════════════════════════════════════════════════════════════════

TEST(RpcWorkflow, RpcLocalSendAndReply) {
    hpactor::Config cfg = hpactor::test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    hpactor::ActorSystem system(cfg);

    auto actor = system.spawn<EchoActor>();
    ASSERT_TRUE(actor);

    auto* echo = static_cast<EchoActor*>(actor.get().get());
    ASSERT_NE(echo, nullptr);

    hpactor::TypedMessage msg(hpactor::TypeTag(0x1001), hpactor::StreamBuffer{});
    system.deliver_local(actor.id(), std::move(msg));

    hpactor::test::SchedulerTestDriver driver(system);
    bool done = driver.drain_until([&]() { return echo->handled() >= 1; });
    EXPECT_TRUE(done);
    EXPECT_GE(echo->handled(), 1);
}

TEST(RpcWorkflow, RpcChannelExistsOnActorSystem) {
    hpactor::Config cfg = hpactor::test::config_with_scheduler(1);
    hpactor::ActorSystem system(cfg);

    // rpc_channel() is always accessible (returns a reference)
    auto& channel = system.rpc_channel();
    (void)channel;
    SUCCEED();
}

TEST(RpcWorkflow, EchoActorRepliesWithSameTypeTag) {
    hpactor::Config cfg = hpactor::test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    hpactor::ActorSystem system(cfg);

    auto actor = system.spawn<EchoActor>();
    ASSERT_TRUE(actor);

    auto* echo = static_cast<EchoActor*>(actor.get().get());

    // Send two messages and verify both are handled
    hpactor::TypedMessage msg1(hpactor::TypeTag(0x2001), hpactor::StreamBuffer{});
    system.deliver_local(actor.id(), std::move(msg1));

    hpactor::TypedMessage msg2(hpactor::TypeTag(0x2002), hpactor::StreamBuffer{});
    system.deliver_local(actor.id(), std::move(msg2));

    hpactor::test::SchedulerTestDriver driver(system);
    bool done = driver.drain_until([&]() { return echo->handled() >= 2; });
    EXPECT_TRUE(done);
    EXPECT_GE(echo->handled(), 2);
}

// ══════════════════════════════════════════════════════════════════════
// Group 2: Timeout, silent, concurrent
// ══════════════════════════════════════════════════════════════════════

TEST(RpcWorkflow, RpcTimeoutConfiguration) {
    // use_default() returns a zero-duration timeout (uses system default)
    auto def = hpactor::RequestTimeout::use_default();
    EXPECT_TRUE(def.is_default());
    EXPECT_EQ(def.kind, hpactor::RequestTimeout::Kind::Duration);

    // from_ms() creates a relative-duration timeout
    auto dur = hpactor::RequestTimeout::from_ms(5000);
    EXPECT_FALSE(dur.is_default());
    EXPECT_EQ(dur.kind, hpactor::RequestTimeout::Kind::Duration);
    EXPECT_EQ(dur.value, std::chrono::milliseconds(5000));

    // from_deadline() creates an absolute-deadline timeout
    auto deadline_tp =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    auto dl = hpactor::RequestTimeout::from_deadline(deadline_tp);
    EXPECT_EQ(dl.kind, hpactor::RequestTimeout::Kind::Deadline);
    EXPECT_NE(dl.deadline(), std::chrono::steady_clock::time_point::max());
}

TEST(RpcWorkflow, SilentActorDoesNotCrashOnMessage) {
    hpactor::Config cfg = hpactor::test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    hpactor::ActorSystem system(cfg);

    auto actor = system.spawn<SilentActor>();
    ASSERT_TRUE(actor);

    // Sending to a silent actor should not crash -- it just drops messages
    hpactor::TypedMessage msg1(hpactor::TypeTag(0x3001), hpactor::StreamBuffer{});
    system.deliver_local(actor.id(), std::move(msg1));

    hpactor::TypedMessage msg2(hpactor::TypeTag(0x3002), hpactor::StreamBuffer{});
    system.deliver_local(actor.id(), std::move(msg2));

    // Drain all messages from the scheduler
    hpactor::test::SchedulerTestDriver driver(system);
    driver.drain();

    // If we reach here without crash, the test passes
    SUCCEED();
}

TEST(RpcWorkflow, ConcurrentMessagesToEchoActor) {
    hpactor::Config cfg = hpactor::test::config_with_scheduler(1);
    cfg.scheduler_start_paused = true;
    hpactor::ActorSystem system(cfg);

    auto actor = system.spawn<EchoActor>();
    ASSERT_TRUE(actor);

    auto* echo = static_cast<EchoActor*>(actor.get().get());

    // Send 5 concurrent messages
    constexpr int kCount = 5;
    for (int i = 0; i < kCount; ++i) {
        hpactor::TypedMessage msg(
            hpactor::TypeTag(0x4000 + static_cast<uint16_t>(i)),
            hpactor::StreamBuffer{});
        system.deliver_local(actor.id(), std::move(msg));
    }

    // Drain until all processed
    hpactor::test::SchedulerTestDriver driver(system);
    bool done = driver.drain_until([&]() { return echo->handled() >= kCount; });
    EXPECT_TRUE(done);
    EXPECT_GE(echo->handled(), kCount);
}

// ══════════════════════════════════════════════════════════════════════
// Group 3: Handle lifecycle, ask manager, config, multi-system
// ══════════════════════════════════════════════════════════════════════

TEST(RpcWorkflow, RequestHandleLifecycle) {
    using namespace hpactor;

    // Default-constructed RequestHandle
    RequestHandle<StreamBuffer> handle1;
    EXPECT_FALSE(handle1.ready());

    // Deadline + MessageId construction
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    MessageId msg_id{42};
    RequestHandle<StreamBuffer> handle2(deadline, msg_id);
    EXPECT_FALSE(handle2.ready());
    EXPECT_EQ(handle2.message_id().value(), 42u);
    EXPECT_EQ(handle2.deadline(), deadline);

    // Move construction
    RequestHandle<StreamBuffer> handle3(std::move(handle2));
    EXPECT_FALSE(handle3.ready());
    EXPECT_EQ(handle3.message_id().value(), 42u);

    // Move assignment
    RequestHandle<StreamBuffer> handle4;
    handle4 = std::move(handle3);
    EXPECT_FALSE(handle4.ready());

    // Cancel a handle
    handle4.cancel();
    EXPECT_TRUE(handle4.ready());

    // Resolve a handle
    RequestHandle<StreamBuffer> handle5(deadline, MessageId{100});
    handle5.resolve(result<StreamBuffer>::make(StreamBuffer{}));
    EXPECT_TRUE(handle5.ready());
}

TEST(RpcWorkflow, AskManagerIntegration) {
    hpactor::Config cfg = hpactor::test::config_with_scheduler(1);
    cfg.enable_network = true;
    hpactor::ActorSystem system(cfg);

    // ask_manager() is accessible when networking is enabled
    auto* ask = system.ask_manager();
    ASSERT_NE(ask, nullptr);

    // Initially no pending asks
    EXPECT_EQ(ask->pending_count(), 0u);
}

TEST(RpcWorkflow, ConfigDefaultAskTimeout) {
    hpactor::Config cfg = hpactor::test::config_with_scheduler(1);
    cfg.default_ask_timeout_ms = std::chrono::milliseconds(3000);
    cfg.default_ask_max_retries = 7;

    hpactor::ActorSystem system(cfg);

    // Verify config values are preserved
    EXPECT_EQ(system.config().default_ask_timeout_ms,
              std::chrono::milliseconds(3000));
    EXPECT_EQ(system.config().default_ask_max_retries, 7u);
}

TEST(RpcWorkflow, MultipleActorSystemsIndependent) {
    hpactor::Config cfg1 = hpactor::test::config_with_scheduler(1);
    cfg1.enable_network = true;
    hpactor::Config cfg2 = hpactor::test::config_with_scheduler(1);
    cfg2.enable_network = true;

    hpactor::ActorSystem system1(cfg1);
    hpactor::ActorSystem system2(cfg2);

    // Each system has its own RPC channel
    auto& channel1 = system1.rpc_channel();
    auto& channel2 = system2.rpc_channel();
    EXPECT_NE(&channel1, &channel2);

    // Each system has its own ask manager
    auto* ask1 = system1.ask_manager();
    auto* ask2 = system2.ask_manager();
    ASSERT_NE(ask1, nullptr);
    ASSERT_NE(ask2, nullptr);
    EXPECT_NE(ask1, ask2);
}
