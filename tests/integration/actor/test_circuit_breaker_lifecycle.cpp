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
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/enqueue_result.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace hpactor;
using namespace hpactor::mailbox;
using namespace std::chrono;

namespace {

/// \brief Simple actor that tracks how many messages it received.
class LifecycleTestActor : public EventBasedActor, public LifecycleActor {
  public:
    LifecycleTestActor(ActorContext* ctx, ActorSystem& sys)
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
        return Behavior{[this](TypedMessage& /*msg*/) { received_++; }};
    }

    uint32_t received_{0};
};

} // namespace

class CircuitBreakerLifecycleTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
        cfg.scheduler_threads = 1;
        system_ = std::make_unique<ActorSystem>(cfg);
    }
    void TearDown() override {
        if (system_) {
            ShutdownOptions opts;
            opts.ingress_timeout = std::chrono::milliseconds(100);
            opts.actor_drain_timeout = std::chrono::milliseconds(100);
            opts.cluster_leave_timeout = std::chrono::milliseconds(100);
            system_->shutdown(opts);
        }
    }

    /// \brief Poll until \p pred returns true or timeout expires.
    template <typename F>
    bool wait_for(F&& pred, milliseconds timeout = milliseconds(5000)) {
        auto deadline = steady_clock::now() + timeout;
        while (steady_clock::now() < deadline) {
            if (pred())
                return true;
            std::this_thread::sleep_for(milliseconds(5));
        }
        return pred();
    }

    std::unique_ptr<ActorSystem> system_;
};

// ── Healthy actor: messages flow, circuit stays closed ─────────────────

TEST_F(CircuitBreakerLifecycleTest, HealthyActorNeverTrips) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;
    policy.observation_window = milliseconds(10000);

    auto target = system_->spawn<LifecycleTestActor>();
    auto* eba =
        static_cast<EventBasedActor*>(system_->get_actor(target.id()).get());
    ASSERT_NE(eba, nullptr);
    eba->configure_quarantine(policy);

    // Send messages — all succeed
    for (int i = 0; i < 20; ++i) {
        TypedMessage msg(TypeTag::User, StreamBuffer{1});
        msg.set_sender_address(target.address());
        system_->deliver_local(target.id(), std::move(msg));
    }

    // Wait for processing
    auto* lta =
        static_cast<LifecycleTestActor*>(system_->get_actor(target.id()).get());
    bool all_received = wait_for([&] { return lta->received_ >= 20; });
    EXPECT_TRUE(all_received);
    EXPECT_EQ(lta->received_, 20u);

    // Circuit should still be closed
    EXPECT_EQ(eba->circuit_breaker()->state, CircuitBreakerState::kClosed);
    EXPECT_EQ(eba->circuit_breaker()->trip_count, 0u);
}

// ── Open circuit rejects messages at admission ─────────────────────────

TEST_F(CircuitBreakerLifecycleTest, OpenCircuitRejectsMessagesIntegration) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;
    policy.cooldown_period = milliseconds(5000);
    policy.observation_window = milliseconds(10000);

    auto target = system_->spawn<LifecycleTestActor>();
    auto* eba =
        static_cast<EventBasedActor*>(system_->get_actor(target.id()).get());
    ASSERT_NE(eba, nullptr);
    eba->configure_quarantine(policy);

    // Trip the circuit manually
    eba->circuit_breaker()->state = CircuitBreakerState::kOpen;
    eba->circuit_breaker()->opened_at = steady_clock::now();

    // Send messages — should be rejected at admission
    for (int i = 0; i < 5; ++i) {
        TypedMessage msg(TypeTag::User, StreamBuffer{1});
        msg.set_sender_address(target.address());
        auto result = system_->try_deliver_local(target.id(), std::move(msg));
        EXPECT_EQ(result.code, EnqueueResultCode::CircuitOpen);
        EXPECT_FALSE(result.accepted());
    }

    // Cooldown not expired — still open
    EXPECT_EQ(eba->circuit_breaker()->state, CircuitBreakerState::kOpen);
}

// ── Cooldown expiry transitions to HalfOpen and admits probe ───────────

TEST_F(CircuitBreakerLifecycleTest, CooldownExpiryAdmitsProbeIntegration) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;
    policy.cooldown_period = milliseconds(100);
    policy.observation_window = milliseconds(10000);

    auto target = system_->spawn<LifecycleTestActor>();
    auto* eba =
        static_cast<EventBasedActor*>(system_->get_actor(target.id()).get());
    ASSERT_NE(eba, nullptr);
    eba->configure_quarantine(policy);

    // Trip the circuit
    eba->circuit_breaker()->state = CircuitBreakerState::kOpen;
    eba->circuit_breaker()->opened_at =
        steady_clock::now() - milliseconds(150); // cooldown expired

    // First message: cooldown expired → transitions to HalfOpen, admitted
    {
        TypedMessage msg(TypeTag::User, StreamBuffer{1});
        msg.set_sender_address(target.address());
        auto result = system_->try_deliver_local(target.id(), std::move(msg));
        EXPECT_EQ(result.code, EnqueueResultCode::Accepted);
    }
    EXPECT_EQ(eba->circuit_breaker()->state, CircuitBreakerState::kHalfOpen);
    EXPECT_TRUE(eba->circuit_breaker()->half_open_probe_in_flight);

    // Second message: probe in flight → rejected
    {
        TypedMessage msg(TypeTag::User, StreamBuffer{1});
        msg.set_sender_address(target.address());
        auto result = system_->try_deliver_local(target.id(), std::move(msg));
        EXPECT_EQ(result.code, EnqueueResultCode::CircuitOpen);
    }
}

// ── Probe success closes circuit ───────────────────────────────────────

TEST_F(CircuitBreakerLifecycleTest, ProbeSuccessClosesCircuitIntegration) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;
    policy.cooldown_period = milliseconds(100);
    policy.observation_window = milliseconds(10000);

    auto target = system_->spawn<LifecycleTestActor>();
    auto* eba =
        static_cast<EventBasedActor*>(system_->get_actor(target.id()).get());
    ASSERT_NE(eba, nullptr);
    eba->configure_quarantine(policy);

    // Trip the circuit
    eba->circuit_breaker()->state = CircuitBreakerState::kOpen;
    eba->circuit_breaker()->trip_count = 2;
    eba->circuit_breaker()->opened_at = steady_clock::now() - milliseconds(150);

    // Send a probe message (admitted via cooldown → HalfOpen)
    {
        TypedMessage msg(TypeTag::User, StreamBuffer{1});
        msg.set_sender_address(target.address());
        system_->deliver_local(target.id(), std::move(msg));
    }

    // Wait for processing: the probe succeeds → circuit closes
    bool closed = wait_for([&] {
        return eba->circuit_breaker()->state == CircuitBreakerState::kClosed;
    });
    EXPECT_TRUE(closed) << "Circuit should close after successful probe";
    EXPECT_EQ(eba->circuit_breaker()->trip_count, 0u);
    EXPECT_FALSE(eba->circuit_breaker()->half_open_probe_in_flight);
}

// ── Repeated trips escalate to quarantine (integration) ────────────────

TEST_F(CircuitBreakerLifecycleTest, RepeatedTripsEscalateToQuarantineIntegration) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 1; // low threshold — easy to trip
    policy.observation_window = milliseconds(1000);
    policy.cooldown_period = milliseconds(100);
    policy.max_circuit_trips = 2; // quarantine after 2nd trip

    auto target = system_->spawn<LifecycleTestActor>();
    auto* eba =
        static_cast<EventBasedActor*>(system_->get_actor(target.id()).get());
    ASSERT_NE(eba, nullptr);
    eba->configure_quarantine(policy);
    auto* lc = static_cast<LifecycleActor*>(
        system_->get_actor(target.id())->as_lifecycle());
    ASSERT_NE(lc, nullptr);

    auto* ft = eba->failure_rate_tracker();
    ASSERT_NE(ft, nullptr);

    // Helper: manually trip the circuit by injecting high failure rate
    auto trip_circuit = [&] {
        for (size_t i = 0; i < FailureRateTracker::kNumBuckets; ++i) {
            ft->failure_buckets[i] = 20; // high rate → EMA exceeds threshold=1
        }
        ft->last_bucket_advance = steady_clock::now();
        ft->bucket_interval_ms = 100;
        eba->record_circuit_breaker_result(false);
    };

    // First trip
    trip_circuit();
    EXPECT_EQ(eba->circuit_breaker()->state, CircuitBreakerState::kOpen);
    EXPECT_EQ(eba->circuit_breaker()->trip_count, 1u);

    // Simulate cooldown expiry + probe failure → second trip
    eba->circuit_breaker()->state = CircuitBreakerState::kHalfOpen;
    eba->circuit_breaker()->half_open_probe_in_flight = true;
    eba->circuit_breaker()->opened_at = steady_clock::now() - policy.cooldown_period;
    trip_circuit(); // probe fails → re-opens → trip_count=2 → quarantine

    EXPECT_EQ(lc->state(), LifecycleState::kQuarantined);
    EXPECT_EQ(lc->quarantine_reason(), QuarantineReason::CircuitBreakerTrip);
}
