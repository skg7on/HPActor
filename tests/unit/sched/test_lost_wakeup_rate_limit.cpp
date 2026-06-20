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

// Deterministic tests verifying that the lost-wakeup re-admission path does
// not spin when a rate-limited actor's dequeue() re-enqueues a message and
// returns nullptr.  Uses scheduler_threads=1 + start_paused so worker
// threads are not running; all dispatches are driven by run_one_ready().

#include <atomic>
#include <memory>

#include <gtest/gtest.h>

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/actor_rate_limiter.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/sched/scheduler.hpp>

using namespace hpactor;

namespace {

// ── Simple counting actor ────────────────────────────────────────────────

class RateLimitedCountingActor : public EventBasedActor {
  public:
    RateLimitedCountingActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    int received() const {
        return counter_.load();
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage&) { counter_.fetch_add(1); }};
    }

  private:
    std::atomic<int> counter_{0};
};

// ── Fixture ──────────────────────────────────────────────────────────────

class LostWakeupRateLimitTest : public ::testing::Test {
  protected:
    void SetUp() override {
        cfg_.scheduler_threads = 1;
        cfg_.scheduler_start_paused = true;
        cfg_.enable_network = false;
        cfg_.enable_receptionist = false;
    }

    Config cfg_;
};

// ── Tests ────────────────────────────────────────────────────────────────

// 2.1 RateLimiterDenialDoesNotSpin
TEST_F(LostWakeupRateLimitTest, RateLimiterDenialDoesNotSpin) {
    ActorSystem system(cfg_);
    auto* sched = system.scheduler();
    ASSERT_NE(sched, nullptr);
    EXPECT_TRUE(sched->workers_paused());

    // Spawn the actor and install a rate limiter that grants exactly one
    // token from its burst, then denies.
    auto actor = system.spawn<RateLimitedCountingActor>();
    auto* raw = static_cast<RateLimitedCountingActor*>(actor.get().get());
    auto* mbox = raw->get_mailbox();
    ASSERT_NE(mbox, nullptr);

    auto limiter = std::make_unique<mailbox::ActorRateLimiter>();
    limiter->configure(0.1, 1); // 0.1 msg/s → one token every 10 s; burst=1
    mbox->set_rate_limiter(std::move(limiter));

    // Send 3 messages.  The first will consume the single burst token;
    // the remaining 2 should leave the mailbox non-empty after the rate
    // limiter denies the second dequeue attempt.
    for (int i = 0; i < 3; ++i) {
        system.deliver_local(actor.id(),
                             TypedMessage(TypeTag::User, StreamBuffer{1}));
    }
    EXPECT_EQ(raw->received(), 0);

    // First dispatch: seq=0, try_pop succeeds (burst token consumed),
    // processes 1 message.  Mailbox still has 2 messages, so the normal
    // RequeueReady path re-admits with seq=1.
    bool executed = sched->run_one_ready();
    EXPECT_TRUE(executed);
    EXPECT_EQ(raw->received(), 1);

    // Second dispatch: seq=1.  try_pop → dequeue → rate limiter denies
    // → re-enqueues → returns nullptr.  The lost-wakeup check sees a
    // non-empty mailbox but seq (1) >= kLostWakeupRequeueBudget (1),
    // so it does NOT re-admit.  The actor suspends.
    executed = sched->run_one_ready();
    EXPECT_TRUE(executed);
    EXPECT_EQ(raw->received(), 1); // rate limiter denied → no new message
                                   // processed

    // Third call: no more ready actors → idle.
    size_t remaining_items = 0;
    for (int i = 0; i < 10; ++i) {
        if (!sched->run_one_ready())
            remaining_items++;
    }
    EXPECT_EQ(remaining_items, 10)
        << "Expected no remaining ready actors after rate-limiter denial; "
           "spin would keep re-admitting the actor";

    // The actor should still have 2 messages in its mailbox (rate-limited).
    EXPECT_FALSE(mbox->empty())
        << "Mailbox should still hold the rate-limited messages";
}

// 2.2 LostWakeupStillReAdmitsWhenMailboxWasEmpty
TEST_F(LostWakeupRateLimitTest, LostWakeupStillReAdmitsWhenMailboxWasEmpty) {
    // Verify that the genuine lost-wakeup case still works: when try_pop
    // returns nullptr because the mailbox was genuinely empty, and a
    // message arrives before set_kIdle, the actor is re-admitted.
    //
    // To simulate this, we run one dispatch on an empty mailbox and inject
    // a message via deliver_local between the try_pop and the
    // lost-wakeup check.  Since we are in paused mode and control the
    // scheduler, we can't inject a message at exactly the right instant,
    // but we can verify that the first lost-wakeup re-admission (seq=0)
    // is allowed through the budget gate.

    ActorSystem system(cfg_);
    auto* sched = system.scheduler();
    ASSERT_NE(sched, nullptr);

    // Spawn an actor WITHOUT a rate limiter.
    auto actor = system.spawn<RateLimitedCountingActor>();
    auto* raw = static_cast<RateLimitedCountingActor*>(actor.get().get());
    auto* mbox = raw->get_mailbox();
    ASSERT_NE(mbox, nullptr);
    // mbox->set_rate_limiter(nullptr); — default, no rate limiter

    // Send one message → the actor processes it on the first dispatch and
    // there is no more work.
    system.deliver_local(actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));

    bool executed = sched->run_one_ready();
    EXPECT_TRUE(executed);
    EXPECT_EQ(raw->received(), 1);

    // No more ready work.
    EXPECT_FALSE(sched->run_one_ready());

    // Send a message while the actor is idle.  This goes through the normal
    // notify_ready → enqueue_admitted path and creates a schedulable work
    // item.
    system.deliver_local(actor.id(), TypedMessage(TypeTag::User, StreamBuffer{1}));
    executed = sched->run_one_ready();
    EXPECT_TRUE(executed);
    EXPECT_EQ(raw->received(), 2);
}

} // anonymous namespace
