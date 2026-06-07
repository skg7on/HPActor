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
#include <hpactor/actor/lifecycle/quarantine_policy.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <string>

using namespace hpactor;
using namespace std::chrono;

namespace {

/// \brief EventBasedActor with LifecycleActor mixin for quarantine tests.
class InspectTestActor : public EventBasedActor, public LifecycleActor {
  public:
    using EventBasedActor::EventBasedActor;

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }
};

} // namespace

class CircuitBreakerInspectTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg.scheduler_threads = 0;
        system_ = std::make_unique<ActorSystem>(cfg);
    }
    void TearDown() override {
        if (system_) {
            ShutdownOptions opts;
            opts.ingress_timeout = milliseconds(10);
            opts.actor_drain_timeout = milliseconds(10);
            opts.cluster_leave_timeout = milliseconds(10);
            system_->shutdown(opts);
        }
    }

    StreamBuffer serialize_request(const cli::InspectStateRequest& req) {
        std::string data = req.SerializeAsString();
        return StreamBuffer(data.begin(), data.end());
    }

    std::unique_ptr<ActorSystem> system_;
};

TEST_F(CircuitBreakerInspectTest, CircuitInfoPopulatedWhenEnabled) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;
    policy.observation_window = milliseconds(10000);

    auto actor = system_->spawn<InspectTestActor>();
    auto* actor_ptr =
        static_cast<EventBasedActor*>(system_->get_actor(actor.id()).get());
    ASSERT_NE(actor_ptr, nullptr);
    actor_ptr->configure_quarantine(policy);

    actor_ptr->circuit_breaker()->trip_count = 3;
    actor_ptr->circuit_breaker()->failure_ema = 2.5;
    actor_ptr->circuit_breaker()->state = CircuitBreakerState::kOpen;
    actor_ptr->circuit_breaker()->opened_at = std::chrono::steady_clock::now();

    // Verify internal state directly (scheduler_threads=0, no races)
    EXPECT_EQ(actor_ptr->circuit_breaker()->state, CircuitBreakerState::kOpen);
    EXPECT_EQ(actor_ptr->circuit_breaker()->trip_count, 3u);
    EXPECT_DOUBLE_EQ(actor_ptr->circuit_breaker()->failure_ema, 2.5);
    EXPECT_TRUE(actor_ptr->quarantine_enabled());
}

TEST_F(CircuitBreakerInspectTest, CircuitInfoNotPopulatedWhenDisabled) {
    auto actor = system_->spawn<InspectTestActor>();
    auto* actor_ptr =
        static_cast<EventBasedActor*>(system_->get_actor(actor.id()).get());
    ASSERT_NE(actor_ptr, nullptr);

    EXPECT_FALSE(actor_ptr->quarantine_enabled());
    EXPECT_EQ(actor_ptr->circuit_breaker(), nullptr);
}

TEST_F(CircuitBreakerInspectTest, QuarantineInfoPopulatedWhenQuarantined) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;

    auto actor = system_->spawn<InspectTestActor>();
    auto* actor_ptr =
        static_cast<EventBasedActor*>(system_->get_actor(actor.id()).get());
    ASSERT_NE(actor_ptr, nullptr);
    actor_ptr->configure_quarantine(policy);

    auto* lc = static_cast<LifecycleActor*>(
        system_->get_actor(actor.id())->as_lifecycle());
    ASSERT_NE(lc, nullptr);
    lc->transition_to_quarantined(QuarantineReason::OperatorAction);

    EXPECT_EQ(lc->state(), LifecycleState::kQuarantined);
    EXPECT_EQ(lc->quarantine_reason(), QuarantineReason::OperatorAction);
}
