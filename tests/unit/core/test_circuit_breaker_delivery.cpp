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
#include <hpactor/actor/lifecycle/quarantine_policy.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/enqueue_result.hpp>

#include <chrono>
#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::mailbox;
using namespace std::chrono;

namespace {

/// \brief Minimal EventBasedActor that does nothing — used as test fixture.
class NoopActor : public EventBasedActor {
  public:
    using EventBasedActor::EventBasedActor;
};

} // namespace

class CircuitBreakerDeliveryTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg.scheduler_threads = 0;
        system_ = std::make_unique<ActorSystem>(cfg);
        target_ = system_->spawn<NoopActor>();
        auto actor = system_->get_actor(target_.id());
        ASSERT_TRUE(actor != nullptr);
        eba_ = static_cast<EventBasedActor*>(actor.get());

        QuarantinePolicy policy;
        policy.enabled = true;
        policy.failure_rate_threshold = 5;
        policy.cooldown_period = milliseconds(30000);
        policy.observation_window = milliseconds(10000);
        eba_->configure_quarantine(policy);
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

    std::unique_ptr<ActorSystem> system_;
    Actor target_;
    EventBasedActor* eba_{nullptr};
};

// ── G1/G4: Open circuit rejects message at admission ────────────────────

TEST_F(CircuitBreakerDeliveryTest, OpenCircuitRejectsMessage) {
    eba_->circuit_breaker()->state = CircuitBreakerState::kOpen;
    eba_->circuit_breaker()->opened_at = std::chrono::steady_clock::now(); // just
                                                                           // opened

    TypedMessage msg(TypeTag::User, StreamBuffer{1});
    auto result = system_->try_deliver_local(target_.id(), std::move(msg));

    EXPECT_EQ(result.code, EnqueueResultCode::CircuitOpen);
    EXPECT_FALSE(result.accepted());
}

// ── G1: Closed circuit admits messages normally ─────────────────────────

TEST_F(CircuitBreakerDeliveryTest, ClosedCircuitAdmitsMessage) {
    // Default state is kClosed
    ASSERT_EQ(eba_->circuit_breaker()->state, CircuitBreakerState::kClosed);

    TypedMessage msg(TypeTag::User, StreamBuffer{1});
    auto result = system_->try_deliver_local(target_.id(), std::move(msg));

    EXPECT_EQ(result.code, EnqueueResultCode::Accepted);
    EXPECT_TRUE(result.accepted());
}

// ── G3: HalfOpen with probe in flight rejects additional messages ───────

TEST_F(CircuitBreakerDeliveryTest, HalfOpenWithProbeInFlightRejects) {
    eba_->circuit_breaker()->state = CircuitBreakerState::kHalfOpen;
    eba_->circuit_breaker()->half_open_probe_in_flight = true;

    TypedMessage msg(TypeTag::User, StreamBuffer{1});
    auto result = system_->try_deliver_local(target_.id(), std::move(msg));

    EXPECT_EQ(result.code, EnqueueResultCode::CircuitOpen);
}

// ── G3: HalfOpen without probe admits one message ──────────────────────

TEST_F(CircuitBreakerDeliveryTest, HalfOpenWithoutProbeAdmitsOne) {
    eba_->circuit_breaker()->state = CircuitBreakerState::kHalfOpen;
    eba_->circuit_breaker()->half_open_probe_in_flight = false;

    TypedMessage msg(TypeTag::User, StreamBuffer{1});
    auto result = system_->try_deliver_local(target_.id(), std::move(msg));

    EXPECT_EQ(result.code, EnqueueResultCode::Accepted);
    EXPECT_TRUE(result.accepted());
    // The probe flag should be set after admission
    EXPECT_TRUE(eba_->circuit_breaker()->half_open_probe_in_flight);
}

// ── G2: Cooldown expiry transitions Open → HalfOpen and admits probe ────

TEST_F(CircuitBreakerDeliveryTest, CooldownExpiryTransitionsOpenToHalfOpen) {
    eba_->circuit_breaker()->state = CircuitBreakerState::kOpen;
    eba_->circuit_breaker()->opened_at =
        std::chrono::steady_clock::now() - milliseconds(31000); // expired
    eba_->circuit_breaker()->half_open_probe_in_flight = false;

    TypedMessage msg(TypeTag::User, StreamBuffer{1});
    auto result = system_->try_deliver_local(target_.id(), std::move(msg));

    // Cooldown expired → should transition to HalfOpen and admit probe
    EXPECT_EQ(eba_->circuit_breaker()->state, CircuitBreakerState::kHalfOpen);
    EXPECT_TRUE(eba_->circuit_breaker()->half_open_probe_in_flight);
    EXPECT_EQ(result.code, EnqueueResultCode::Accepted);
}

// ── G2: Cooldown not expired keeps circuit open ─────────────────────────

TEST_F(CircuitBreakerDeliveryTest, CooldownNotExpiredKeepsOpen) {
    eba_->circuit_breaker()->state = CircuitBreakerState::kOpen;
    eba_->circuit_breaker()->opened_at =
        std::chrono::steady_clock::now() - milliseconds(100); // not expired
    eba_->circuit_breaker()->half_open_probe_in_flight = false;

    TypedMessage msg(TypeTag::User, StreamBuffer{1});
    auto result = system_->try_deliver_local(target_.id(), std::move(msg));

    // Cooldown not expired → remain Open and reject
    EXPECT_EQ(eba_->circuit_breaker()->state, CircuitBreakerState::kOpen);
    EXPECT_EQ(result.code, EnqueueResultCode::CircuitOpen);
}

// ── Zero-overhead: quarantine disabled → no circuit breaker check ───────

TEST_F(CircuitBreakerDeliveryTest, NoQuarantineNoOverhead) {
    // Reconfigure target without quarantine
    QuarantinePolicy disabled;
    disabled.enabled = false;
    eba_->configure_quarantine(disabled);

    // Manually set circuit state to Open — should be ignored since
    // quarantine is disabled, so the circuit breaker pointer is null.
    ASSERT_EQ(eba_->circuit_breaker(), nullptr);
    ASSERT_FALSE(eba_->quarantine_enabled());

    TypedMessage msg(TypeTag::User, StreamBuffer{1});
    auto result = system_->try_deliver_local(target_.id(), std::move(msg));

    EXPECT_EQ(result.code, EnqueueResultCode::Accepted);
    EXPECT_TRUE(result.accepted());
}
