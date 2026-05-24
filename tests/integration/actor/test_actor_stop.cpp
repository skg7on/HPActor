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
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/messages.pb.h>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace hpactor;

// Helper: poll until condition is true or timeout expires
template <typename Fn>
static bool poll_until(Fn&& condition, int timeout_ms = 2000) {
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

// StopTestActor: lifecycle actor with ImmediateStop policy
class StopTestActor : public EventBasedActor, public LifecycleActor {
  public:
    StopTestActor(ActorContext* ctx, ActorSystem& sys, DrainConfig drain_cfg = {})
        : EventBasedActor(ctx, sys) {
        set_drain_config(drain_cfg);
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    bool exit_called() const {
        return exit_called_;
    }
    int down_count() const {
        return down_count_;
    }

    void on_exit() override {
        exit_called_ = true;
        EventBasedActor::on_exit();
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == TypeTag::DownMsg) {
                ++down_count_;
            }
        }};
    }

  private:
    bool exit_called_ = false;
    int down_count_ = 0;
};

// NonLifecycleActor: no lifecycle, just counts exit calls
class NonLifecycleTestActor : public EventBasedActor {
  public:
    NonLifecycleTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    bool exit_called() const {
        return exit_called_;
    }

    void on_exit() override {
        exit_called_ = true;
        EventBasedActor::on_exit();
    }

  private:
    bool exit_called_ = false;
};

class ActorStopIntegrationTest : public ::testing::Test {
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

TEST_F(ActorStopIntegrationTest, StopAsyncTransitionsToStopped) {
    auto target_ref =
        system_->spawn<StopTestActor>(DrainConfig{DrainPolicy::ImmediateStop});
    auto* target = static_cast<StopTestActor*>(target_ref.get().get());
    auto* lc = target->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    EXPECT_EQ(lc->state(), LifecycleState::kActive);

    // Spawn a monitor actor linked to the target
    auto monitor_ref =
        system_->spawn<StopTestActor>(DrainConfig{DrainPolicy::ImmediateStop});
    auto* monitor = static_cast<StopTestActor*>(monitor_ref.get().get());
    target->context()->add_monitored(monitor->address());

    // Create an ActorContext with system pointer to call stop()
    ActorContext ctx(Actor{}, system_.get());

    // Call stop()
    ctx.stop(target_ref.id());

    // Target should be kStopped immediately (ImmediateStop is synchronous)
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);
    EXPECT_TRUE(target->exit_called());

    // Monitor mailbox should contain a DownMsg
    auto* mbox = system_->get_mailbox(monitor_ref.id());
    ASSERT_NE(mbox, nullptr);
    bool has_down = false;
    TypedMessage msg;
    while (mbox->try_pop(msg)) {
        if (msg.type_id() == TypeTag::DownMsg) {
            has_down = true;
        }
    }
    EXPECT_TRUE(has_down);
}

TEST_F(ActorStopIntegrationTest, StopSyncBlocksUntilStopped) {
    auto target_ref =
        system_->spawn<StopTestActor>(DrainConfig{DrainPolicy::ImmediateStop});
    auto* target = static_cast<StopTestActor*>(target_ref.get().get());
    auto* lc = target->as_lifecycle();

    ActorContext ctx(Actor{}, system_.get());

    auto result = ctx.stop_sync(target_ref.id(), std::chrono::milliseconds(5000));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(lc->state(), LifecycleState::kStopped);
}

TEST_F(ActorStopIntegrationTest, StopSyncTimeoutReturnsError) {
    // Use default DrainPolicy::Drain with 30s drain timeout.
    auto target_ref = system_->spawn<StopTestActor>();
    auto* target = static_cast<StopTestActor*>(target_ref.get().get());
    auto* lc = target->as_lifecycle();
    ASSERT_NE(lc, nullptr);

    // Inject messages so drain can't complete instantly
    auto* mailbox = target->get_mailbox();
    for (int i = 0; i < 5; ++i) {
        auto* node = static_cast<TypedMessage*>(mem::allocate(
            mem::RegionType::kMessage, sizeof(TypedMessage), target->id()));
        new (node) TypedMessage(TypeTag(0x1001), StreamBuffer{});
        node->set_sender_address(ActorAddress{});
        mailbox->inject_for_test(node);
    }

    ActorContext ctx(Actor{}, system_.get());

    auto result = ctx.stop_sync(target_ref.id(), std::chrono::milliseconds(10));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), errors::timeout);
    auto state = lc->state();
    EXPECT_NE(state, LifecycleState::kStopped);
}

TEST_F(ActorStopIntegrationTest, StopNoLifecycleCallsOnExitDirectly) {
    auto target_ref = system_->spawn<NonLifecycleTestActor>();
    auto* target = static_cast<NonLifecycleTestActor*>(target_ref.get().get());
    ASSERT_EQ(target->as_lifecycle(), nullptr);
    EXPECT_FALSE(target->exit_called());

    ActorContext ctx(Actor{}, system_.get());

    ctx.stop(target_ref.id());

    EXPECT_TRUE(target->exit_called());
}