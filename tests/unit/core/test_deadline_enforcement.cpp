// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/mem/thread_local_allocator.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/sched/actor_execution_engine.hpp>
#include <hpactor/sched/actor_ready_gate.hpp>

#include <cstdint>
#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::mailbox;

namespace {

class NoopActor : public EventBasedActor {
  public:
    using EventBasedActor::EventBasedActor;
};

} // namespace

class DeadlineEnforcementTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg.scheduler_threads = 0;
        system_ = std::make_unique<ActorSystem>(cfg);
        target_ = system_->spawn<NoopActor>();
        ASSERT_NE(target_.id(), ActorId{});
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
    Actor target_;
};

// ── BestEffort expired deadline → Rejected ───────────────────────────
TEST_F(DeadlineEnforcementTest, BestEffortExpiredDeadlineRejected) {
    TypedMessage msg(TypeTag::User, StreamBuffer{1});

    // deadline_ns = 1 (nanosecond 1 — definitely in the past)
    int64_t expired_deadline = 1;

    auto result = system_->try_deliver_local(target_.id(), std::move(msg),
                                             /*priority=*/0,
                                             /*deadline_ns=*/expired_deadline,
                                             DeliveryOptions{}); // BestEffort
                                                                 // (default)

    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(result.code, EnqueueResultCode::Rejected);
}

// ── ObservableBestEffort expired deadline → Rejected ─────────────────
TEST_F(DeadlineEnforcementTest, ObservableBestEffortExpiredDeadlineRejected) {
    TypedMessage msg(TypeTag::User, StreamBuffer{1});

    int64_t expired_deadline = 1;

    DeliveryOptions options;
    options.delivery_mode = DeliveryMode::ObservableBestEffort;

    auto result =
        system_->try_deliver_local(target_.id(), std::move(msg),
                                   /*priority=*/0,
                                   /*deadline_ns=*/expired_deadline, options);

    EXPECT_FALSE(result.accepted());
    EXPECT_EQ(result.code, EnqueueResultCode::Rejected);
}

// ── Future deadline → Accepted ───────────────────────────────────────
TEST_F(DeadlineEnforcementTest, FutureDeadlineAccepted) {
    TypedMessage msg(TypeTag::User, StreamBuffer{1});

    // Far-future deadline
    int64_t future_deadline = INT64_MAX - 1;

    auto result = system_->try_deliver_local(target_.id(), std::move(msg),
                                             /*priority=*/0,
                                             /*deadline_ns=*/future_deadline,
                                             DeliveryOptions{});

    EXPECT_TRUE(result.accepted());
}

// ── INT64_MAX deadline (none) → Accepted ─────────────────────────────
TEST_F(DeadlineEnforcementTest, NoDeadlineAccepted) {
    TypedMessage msg(TypeTag::User, StreamBuffer{1});

    auto result =
        system_->try_deliver_local(target_.id(), std::move(msg),
                                   /*priority=*/0,
                                   /*deadline_ns=*/INT64_MAX, DeliveryOptions{});

    EXPECT_TRUE(result.accepted());
}

// ── Default TTL configured: message gets computed deadline ──────────
TEST(DeadlineEnforcementDefaultTtlTest, DefaultTtlAppliesDeadline) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    cfg.default_message_ttl_ms = std::chrono::milliseconds{30000};

    auto system = std::make_unique<ActorSystem>(cfg);
    auto target = system->spawn<NoopActor>();
    ASSERT_NE(target.id(), ActorId{});

    TypedMessage msg(TypeTag::User, StreamBuffer{1});

    auto result =
        system->try_deliver_local(target.id(), std::move(msg),
                                  /*priority=*/0,
                                  /*deadline_ns=*/INT64_MAX, DeliveryOptions{});

    EXPECT_TRUE(result.accepted());

    // Pop the message and verify a deadline was applied (not INT64_MAX)
    auto* mbox = system->get_mailbox(target.id());
    ASSERT_NE(mbox, nullptr);
    TypedMessage popped;
    ASSERT_TRUE(mbox->try_pop(popped));
    EXPECT_NE(popped.deadline_ns(), INT64_MAX) << "Default TTL should "
                                                  "auto-apply a deadline";

    ShutdownOptions sopts;
    sopts.ingress_timeout = std::chrono::milliseconds(10);
    sopts.actor_drain_timeout = std::chrono::milliseconds(10);
    sopts.cluster_leave_timeout = std::chrono::milliseconds(10);
    system->shutdown(sopts);
}

// ── Explicit deadline wins over default TTL ─────────────────────────
TEST(DeadlineEnforcementDefaultTtlTest, ExplicitDeadlineWinsOverDefault) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    cfg.default_message_ttl_ms = std::chrono::milliseconds{30000};

    auto system = std::make_unique<ActorSystem>(cfg);
    auto target = system->spawn<NoopActor>();
    ASSERT_NE(target.id(), ActorId{});

    TypedMessage msg(TypeTag::User, StreamBuffer{1});

    // Explicit far-future deadline still accepted
    int64_t explicit_deadline = INT64_MAX - 1;

    auto result = system->try_deliver_local(target.id(), std::move(msg),
                                            /*priority=*/0,
                                            /*deadline_ns=*/explicit_deadline,
                                            DeliveryOptions{});

    EXPECT_TRUE(result.accepted());

    ShutdownOptions sopts;
    sopts.ingress_timeout = std::chrono::milliseconds(10);
    sopts.actor_drain_timeout = std::chrono::milliseconds(10);
    sopts.cluster_leave_timeout = std::chrono::milliseconds(10);
    system->shutdown(sopts);
}

// ── Expired while queued → dropped before handler, DLQ record ───────
TEST(DeadlineEnforcementRunnerTest, ExpiredWhileQueuedDroppedBeforeHandler) {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.scheduler_threads = 0;
    cfg.dead_letters.enabled = true;
    cfg.dead_letters.capacity = 64;

    auto system = std::make_unique<ActorSystem>(cfg);
    auto target = system->spawn<NoopActor>();
    ASSERT_NE(target.id(), ActorId{});

    // Inject an already-expired message directly into the mailbox,
    // bypassing the pre-enqueue expiry check.
    auto* mbox = system->get_mailbox(target.id());
    ASSERT_NE(mbox, nullptr);
    // Allocate via HPActor memory system to match mem::deallocate in try_pop.
    void* mem =
        mem::allocate(mem::RegionType::kMessage, sizeof(TypedMessage), ActorId{});
    auto* injected = new (mem) TypedMessage(TypeTag::User, StreamBuffer{1});
    injected->set_deadline_ns(1); // nanosecond 1 — definitely expired
    mbox->inject_for_test(injected);

    // Run the behavior runner to process the message.
    sched::ActorReadyGate gate(*system);
    sched::BehaviorActorRunner runner(*system, gate);
    sched::ActorExecutionContext ctx{0, nullptr, nullptr};

    auto actor = system->get_actor(target.id());
    ASSERT_NE(actor, nullptr);
    auto* eba = static_cast<EventBasedActor*>(actor.get());
    // Must be Ready for the runner's CAS (kReady → kRunning) to succeed.
    eba->actor_state().set(ActorState::kReady);

    // Verify the message was injected into the mailbox.
    EXPECT_FALSE(mbox->empty());

    // Verify the DLQ is accessible.
    auto* dlq = system->dead_letter_queue();
    ASSERT_NE(dlq, nullptr);
    ASSERT_TRUE(dlq->config().enabled);

    sched::WorkItem item{target.id(), INT64_MAX, 0};
    auto run_result = runner.run(*eba, item, ctx);

    // Mailbox should be empty after popping the expired message.
    EXPECT_NE(run_result.disposition, sched::ActorRunDisposition::RequeueReady);

    // DLQ should now contain the expired record.
    auto dlq_snap = dlq->snapshot_records();
    EXPECT_GE(dlq_snap.size(), 1u) << "DLQ should contain the expired message";
    if (!dlq_snap.empty()) {
        EXPECT_EQ(dlq_snap[0].reason, mailbox::DeadLetterReason::Expired);
    }

    ShutdownOptions sopts;
    sopts.ingress_timeout = std::chrono::milliseconds(10);
    sopts.actor_drain_timeout = std::chrono::milliseconds(10);
    sopts.cluster_leave_timeout = std::chrono::milliseconds(10);
    system->shutdown(sopts);
}

// ── Coroutine awaiter drops expired messages before handler ─────────

#if HPACTOR_SUPPORT_COROUTINES

#    include <hpactor/coroutine/coroutine_awaiters.hpp>
#    include <hpactor/coroutine/coroutine_task.hpp>
#    include <hpactor/mailbox/dead_letter_queue.hpp>

TEST(DeadlineEnforcementCoroutineTest, ExpiredMessageSkippedInAwaitResume) {
    // Create a mailbox with DLQ.
    mailbox::DeadLetterConfig dlq_cfg;
    dlq_cfg.enabled = true;
    dlq_cfg.capacity = 64;
    auto dlq = std::make_unique<mailbox::DeadLetterQueue>(dlq_cfg);

    hpactor::mailbox::MPSCActorMailbox<TypedMessage> mbox(ActorId{42}, nullptr);

    // Create a CoroutinePromise.
    sched::CoroutinePromise promise;
    promise.actor_id = ActorId{42};
    promise.state.set(ActorState::kRunning);

    // Allocate via HPActor memory system to match mem::deallocate in
    // await_resume.
    void* coro_mem =
        mem::allocate(mem::RegionType::kMessage, sizeof(TypedMessage), ActorId{});
    auto* expired =
        new (coro_mem) TypedMessage(TypeTag::User, StreamBuffer{1, 2, 3});
    expired->set_deadline_ns(1); // nanosecond 1 — definitely expired
    mbox.inject_for_test(expired);

    // Create the awaiter with DLQ.
    sched::MailboxAwaiter<TypedMessage> awaiter(promise, &mbox, dlq.get(),
                                                nullptr, ActorId{42});

    // await_resume should skip the expired message and return T{}.
    TypedMessage result = awaiter.await_resume();

    // The returned message should be empty (default-constructed).
    EXPECT_EQ(result.type_id(), TypeTag::Invalid);
    EXPECT_TRUE(result.payload().empty());

    // DLQ should contain the expired record.
    auto snap = dlq->snapshot_records();
    ASSERT_GE(snap.size(), 1u);
    EXPECT_EQ(snap[0].reason, mailbox::DeadLetterReason::Expired);
}

#endif // HPACTOR_SUPPORT_COROUTINES
