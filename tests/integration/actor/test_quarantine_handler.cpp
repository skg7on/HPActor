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
#include <hpactor/actor/lifecycle/circuit_breaker.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/cli_messages.pb.h>

#include <gtest/gtest.h>
#include <scheduler_test_driver.hpp>

using namespace hpactor;
using namespace hpactor::cli;

namespace {

class QuarantineTestActor : public EventBasedActor, public LifecycleActor {
  public:
    QuarantineTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }
};

} // namespace

class QuarantineHandlerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg.scheduler_threads = 1;
        cfg.scheduler_start_paused = true;
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

TEST_F(QuarantineHandlerTest, QuarantineTransitionsToQuarantined) {
    hpactor::test::SchedulerTestDriver driver(*system_);

    auto target = system_->spawn<QuarantineTestActor>();
    auto* lc = static_cast<LifecycleActor*>(
        system_->get_actor(target.id())->as_lifecycle());
    ASSERT_NE(lc, nullptr);
    ASSERT_NE(lc->state(), LifecycleState::kQuarantined);

    // Send quarantine request
    QuarantineRequest req;
    req.set_target_actor_id(target.id().value());
    req.set_unquarantine(false);

    TypedMessage msg(TypeTag::QuarantineRequestTag, req);
    msg.set_sender_address(target.address());
    system_->deliver_local(target.id(), std::move(msg));

    bool changed = driver.drain_until(
        [&] { return lc->state() == LifecycleState::kQuarantined; });
    EXPECT_TRUE(changed);
    EXPECT_EQ(lc->state(), LifecycleState::kQuarantined);
}

TEST_F(QuarantineHandlerTest, UnquarantineTransitionsBackToActive) {
    hpactor::test::SchedulerTestDriver driver(*system_);

    auto target = system_->spawn<QuarantineTestActor>();
    auto* lc = static_cast<LifecycleActor*>(
        system_->get_actor(target.id())->as_lifecycle());
    ASSERT_NE(lc, nullptr);

    lc->transition_to_quarantined(QuarantineReason::OperatorAction);
    ASSERT_EQ(lc->state(), LifecycleState::kQuarantined);

    QuarantineRequest req;
    req.set_target_actor_id(target.id().value());
    req.set_unquarantine(true);

    TypedMessage msg(TypeTag::QuarantineRequestTag, req);
    msg.set_sender_address(target.address());
    system_->deliver_local(target.id(), std::move(msg));

    bool changed =
        driver.drain_until([&] { return lc->state() == LifecycleState::kActive; });
    EXPECT_TRUE(changed);
    EXPECT_EQ(lc->state(), LifecycleState::kActive);
}

TEST_F(QuarantineHandlerTest, UnquarantineResetsCircuitBreaker) {
    hpactor::test::SchedulerTestDriver driver(*system_);

    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;

    auto target = system_->spawn<QuarantineTestActor>();
    auto* eba =
        static_cast<EventBasedActor*>(system_->get_actor(target.id()).get());
    ASSERT_NE(eba, nullptr);
    eba->configure_quarantine(policy);

    auto* lc = static_cast<LifecycleActor*>(
        system_->get_actor(target.id())->as_lifecycle());
    ASSERT_NE(lc, nullptr);

    eba->circuit_breaker()->state = CircuitBreakerState::kOpen;
    eba->circuit_breaker()->trip_count = 3;
    eba->circuit_breaker()->half_open_probe_in_flight = true;
    lc->transition_to_quarantined(QuarantineReason::CircuitBreakerTrip);

    QuarantineRequest req;
    req.set_target_actor_id(target.id().value());
    req.set_unquarantine(true);

    TypedMessage msg(TypeTag::QuarantineRequestTag, req);
    msg.set_sender_address(target.address());
    system_->deliver_local(target.id(), std::move(msg));

    bool changed =
        driver.drain_until([&] { return lc->state() == LifecycleState::kActive; });
    EXPECT_TRUE(changed) << "Expected Actor to unquarantine and become Active";
    EXPECT_EQ(lc->state(), LifecycleState::kActive);
    EXPECT_EQ(eba->circuit_breaker()->state, CircuitBreakerState::kClosed);
    EXPECT_EQ(eba->circuit_breaker()->trip_count, 0u);
    EXPECT_FALSE(eba->circuit_breaker()->half_open_probe_in_flight);
}
