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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <gtest/gtest.h>
#include <unordered_map>
#include <unordered_set>

using namespace hpactor;

// Mock scheduler that captures timer callbacks
class MockScheduler : public sched::IScheduler {
  public:
    sched::TimerHandle schedule_after(sched::timer_callback cb, int64_t) override {
        uint64_t id = next_id_++;
        callbacks_[id] = std::move(cb);
        return sched::TimerHandle{id};
    }
    sched::TimerHandle
    schedule_every(sched::timer_callback cb, int64_t interval_ns) override {
        return schedule_after(std::move(cb), interval_ns);
    }
    void cancel_timer(sched::TimerHandle handle) override {
        cancelled_.insert(handle.value());
        callbacks_.erase(handle.value());
    }
    void notify_ready(ActorId, uint8_t, int64_t) override {}
    void notify_idle(ActorId) override {}
    void yield(ActorId, uint8_t) override {}
    void start() override {}
    void stop() override {}
    size_t worker_count() const override {
        return 1;
    }
    bool is_running() const override {
        return true;
    }
    void register_dedicated_thread(ActorId, int) override {}
    void register_dedicated_pool(ActorId, uint32_t) override {}
    void unregister_dedicated(ActorId) override {}

    // Invoke a previously scheduled timer callback by its timer ID.
    void invoke_timer(uint64_t id) {
        auto it = callbacks_.find(id);
        if (it != callbacks_.end()) {
            auto cb = std::move(it->second);
            callbacks_.erase(it);
            cb();
        }
    }

    bool is_cancelled(uint64_t id) const {
        return cancelled_.count(id) > 0;
    }

    size_t pending_timers() const {
        return callbacks_.size();
    }

    std::unordered_map<uint64_t, sched::timer_callback> callbacks_;
    std::unordered_set<uint64_t> cancelled_;
    uint64_t next_id_ = 1;
};

// Test actor
class DrainTimeoutTestActor : public EventBasedActor, public LifecycleActor {
  public:
    int handler_count = 0;

    DrainTimeoutTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (static_cast<uint32_t>(msg.type_id()) >= 0x1000) {
                handler_count++;
            }
        }};
    }
};

// Helpers
static void inject_user_message(EventBasedActor* actor) {
    auto* mailbox = actor->get_mailbox();
    ASSERT_NE(mailbox, nullptr);

    static_assert(sizeof(TypedMessage) <= 1024, "TypedMessage must fit in 1024 "
                                                "bytes for stack allocation");

    auto* node = static_cast<TypedMessage*>(mem::allocate(
        mem::RegionType::kMessage, sizeof(TypedMessage), actor->id()));
    new (node) TypedMessage(TypeTag(0x1001), StreamBuffer{});
    node->set_sender_address(ActorAddress{});

    mailbox->inject_for_test(node);
}

static uint32_t dlq_depth(ActorSystem& system) {
    auto snapshot = system.dead_letter_snapshot();
    return snapshot.depth;
}

// Fixture
class DrainTimeoutIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.scheduler_threads = 0;
        cfg.enable_network = false;
        system_ = std::make_unique<ActorSystem>(cfg);
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

TEST_F(DrainTimeoutIntegrationTest, TimeoutForcesTransition) {
    auto ref = system_->spawn<DrainTimeoutTestActor>();
    auto* actor = static_cast<DrainTimeoutTestActor*>(ref.get().get());

    auto* lc = actor->as_lifecycle();
    ASSERT_NE(lc, nullptr);

    MockScheduler mock_sched;
    actor->set_scheduler(&mock_sched);

    lc->set_drain_config(
        DrainConfig{DrainPolicy::Drain, std::chrono::milliseconds{1}});

    for (int i = 0; i < 10; ++i) {
        inject_user_message(actor);
    }

    bool ok = lc->transition(LifecycleState::kDraining);
    ASSERT_TRUE(ok);

    actor->start_drain_timer();
    EXPECT_EQ(mock_sched.pending_timers(), 1u);

    uint64_t timer_id = mock_sched.next_id_ - 1;
    mock_sched.invoke_timer(timer_id);

    EXPECT_EQ(lc->state(), LifecycleState::kStopped);
    EXPECT_EQ(actor->handler_count, 0);
    EXPECT_EQ(dlq_depth(*system_), 10u);
    EXPECT_TRUE(actor->mailbox_is_empty());
}

TEST_F(DrainTimeoutIntegrationTest, CompletesBeforeTimeoutCancelsTimer) {
    auto ref = system_->spawn<DrainTimeoutTestActor>();
    auto* actor = static_cast<DrainTimeoutTestActor*>(ref.get().get());

    auto* lc = actor->as_lifecycle();
    ASSERT_NE(lc, nullptr);

    MockScheduler mock_sched;
    actor->set_scheduler(&mock_sched);

    lc->set_drain_config(
        DrainConfig{DrainPolicy::Drain, std::chrono::milliseconds{5000}});

    inject_user_message(actor);

    bool ok = lc->transition(LifecycleState::kDraining);
    ASSERT_TRUE(ok);

    actor->start_drain_timer();
    uint64_t timer_id = mock_sched.next_id_ - 1;
    EXPECT_EQ(mock_sched.pending_timers(), 1u);

    // Process the single message — this should complete drain naturally.
    TypedMessage msg;
    auto* mailbox = actor->get_mailbox();
    bool found = mailbox->try_pop(msg);
    ASSERT_TRUE(found);
    actor->receive(msg);

    EXPECT_EQ(actor->handler_count, 1);
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);
    EXPECT_TRUE(mock_sched.is_cancelled(timer_id));
    EXPECT_EQ(mock_sched.pending_timers(), 0u);
    EXPECT_TRUE(actor->mailbox_is_empty());
}