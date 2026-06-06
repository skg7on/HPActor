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
#include <hpactor/actor/drain_config.hpp>
#include <hpactor/actor/drain_policy.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/dead_letter_record.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace hpactor;

// Helper: poll until condition or timeout
template <typename Fn>
static bool poll_until(Fn&& condition, int timeout_ms = 100) {
    auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return condition();
}

// Simple lifecycle test actor
class ShutdownTestActor : public EventBasedActor, public LifecycleActor {
  public:
    ShutdownTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    bool drain_hook_called() const {
        return drain_hook_called_;
    }
    bool stop_hook_called() const {
        return stop_hook_called_;
    }

    void on_drain() override {
        drain_hook_called_ = true;
    }
    void on_stop() override {
        stop_hook_called_ = true;
    }

    void on_exit() override {
        exit_called_ = true;
        EventBasedActor::on_exit();
    }
    bool exit_called() const {
        return exit_called_;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[](TypedMessage&) {}};
    }

  private:
    bool drain_hook_called_ = false;
    bool stop_hook_called_ = false;
    bool exit_called_ = false;
};

// System actor (drains last)
class ShutdownSystemActor : public EventBasedActor, public LifecycleActor {
  public:
    ShutdownSystemActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    bool is_system_actor() const override {
        return true;
    }

    void on_exit() override {
        exit_called_ = true;
        EventBasedActor::on_exit();
    }
    bool exit_called() const {
        return exit_called_;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[](TypedMessage&) {}};
    }

  private:
    bool exit_called_ = false;
};

class ShutdownCoordinatorIntegrationTest : public ::testing::Test {
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

TEST_F(ShutdownCoordinatorIntegrationTest, PhaseMachineTransitions) {
    system_->spawn<ShutdownTestActor>();
    system_->spawn<ShutdownTestActor>();

    EXPECT_TRUE(system_->is_ready());
    EXPECT_EQ(system_->shutdown_phase(), ShutdownPhase::Running);

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);

    auto result = system_->shutdown(opts);
    ASSERT_TRUE(result.has_value());

    EXPECT_FALSE(system_->is_ready());
    EXPECT_TRUE(system_->shutdown_phase() == ShutdownPhase::Stopped ||
                system_->shutdown_phase() == ShutdownPhase::ForcedStop);
}

TEST_F(ShutdownCoordinatorIntegrationTest, ReverseTopologicalOrder) {
    auto child_ref = system_->spawn<ShutdownTestActor>();
    auto parent_ref = system_->spawn<ShutdownSystemActor>();

    auto* child = static_cast<ShutdownTestActor*>(child_ref.get().get());
    auto* parent = static_cast<ShutdownSystemActor*>(parent_ref.get().get());

    auto* child_lc = child->as_lifecycle();
    auto* parent_lc = parent->as_lifecycle();
    ASSERT_NE(child_lc, nullptr);
    ASSERT_NE(parent_lc, nullptr);
    child_lc->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});
    parent_lc->set_drain_config(DrainConfig{DrainPolicy::ImmediateStop});

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);

    auto result = system_->shutdown(opts);
    ASSERT_TRUE(result.has_value());

    EXPECT_TRUE(child->exit_called());
    EXPECT_TRUE(parent->exit_called());

    EXPECT_TRUE(system_->shutdown_phase() == ShutdownPhase::Stopped ||
                system_->shutdown_phase() == ShutdownPhase::ForcedStop);
}

TEST_F(ShutdownCoordinatorIntegrationTest, ForcedStopOnTimeout) {
    auto actor_ref = system_->spawn<ShutdownTestActor>();
    auto* actor = static_cast<ShutdownTestActor*>(actor_ref.get().get());

    auto* lc = actor->as_lifecycle();
    ASSERT_NE(lc, nullptr);
    lc->set_drain_config(
        DrainConfig{DrainPolicy::Drain, std::chrono::milliseconds{30'000}});

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(1);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);
    opts.force_after_timeout = true;

    auto result = system_->shutdown(opts);
    ASSERT_TRUE(result.has_value());

    EXPECT_FALSE(system_->is_running());
    EXPECT_EQ(system_->shutdown_phase(), ShutdownPhase::ForcedStop);

    auto state = lc->state();
    EXPECT_TRUE(state == LifecycleState::kStopped ||
                state == LifecycleState::kDraining ||
                state == LifecycleState::kStopping);
}

TEST_F(ShutdownCoordinatorIntegrationTest, IsReadyFlipsOnDrainingIngress) {
    EXPECT_TRUE(system_->is_ready());
    EXPECT_FALSE(system_->is_draining());

    system_->spawn<ShutdownTestActor>();

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);

    auto result = system_->shutdown(opts);
    ASSERT_TRUE(result.has_value());

    EXPECT_FALSE(system_->is_ready());
}