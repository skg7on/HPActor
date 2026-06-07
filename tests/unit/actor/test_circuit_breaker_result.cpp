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
#include <hpactor/actor/lifecycle/failure_rate_tracker.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/actor/lifecycle/quarantine_policy.hpp>
#include <hpactor/adt/mpsc_ring_buffer.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/metrics/metrics_event.hpp>

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace hpactor;
using namespace std::chrono;

namespace {

/// \brief Minimal EventBasedActor for circuit breaker unit tests.
class NoopActor : public EventBasedActor {
  public:
    using EventBasedActor::EventBasedActor;
};

/// \brief EventBasedActor with LifecycleActor mixin — needed for
///        quarantine escalation tests.
class LifecycleNoopActor : public EventBasedActor, public LifecycleActor {
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

class CircuitBreakerResultTest : public ::testing::Test {
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

    /// \brief Spawn a NoopActor and configure it with the given policy.
    void setup_actor(const QuarantinePolicy& policy) {
        target_ = system_->spawn<NoopActor>();
        auto actor = system_->get_actor(target_.id());
        ASSERT_TRUE(actor != nullptr);
        eba_ = static_cast<EventBasedActor*>(actor.get());
        eba_->configure_quarantine(policy);
        cb_ = eba_->circuit_breaker();
        ft_ = eba_->failure_rate_tracker();
        ASSERT_NE(cb_, nullptr);
        ASSERT_NE(ft_, nullptr);
    }

    /// \brief Spawn a LifecycleNoopActor and configure it.
    void setup_lifecycle_actor(const QuarantinePolicy& policy) {
        target_ = system_->spawn<LifecycleNoopActor>();
        auto actor = system_->get_actor(target_.id());
        ASSERT_TRUE(actor != nullptr);
        eba_ = static_cast<EventBasedActor*>(actor.get());
        lc_ = static_cast<LifecycleActor*>(actor->as_lifecycle());
        ASSERT_NE(lc_, nullptr);
        eba_->configure_quarantine(policy);
        cb_ = eba_->circuit_breaker();
        ft_ = eba_->failure_rate_tracker();
        ASSERT_NE(cb_, nullptr);
    }

    /// \brief Populate failure rate tracker buckets so that
    ///        \c failure_rate(window_ms) exceeds \p target_rate.
    ///
    /// Places all failures in bucket 0 and sets \c last_bucket_advance
    /// so the buckets are not cleared.
    void fill_failure_rate(double target_rate, uint32_t window_ms) {
        // rate = total / (window_ms / 1000.0)  →  total = target_rate *
        // window_ms / 1000
        uint32_t needed = static_cast<uint32_t>(target_rate * window_ms / 1000.0);
        for (uint32_t i = 0; i < needed && i < FailureRateTracker::kNumBuckets;
             ++i) {
            ft_->failure_buckets[i] = 1;
        }
        // If we need more than kNumBuckets, stack them in bucket 0
        if (needed > FailureRateTracker::kNumBuckets) {
            ft_->failure_buckets[0] = static_cast<uint32_t>(
                needed - (FailureRateTracker::kNumBuckets - 1));
        }
        ft_->last_bucket_advance = std::chrono::steady_clock::now();
        ft_->bucket_interval_ms = window_ms / FailureRateTracker::kNumBuckets;
        if (ft_->bucket_interval_ms == 0) {
            ft_->bucket_interval_ms = 1;
        }
    }

    /// \brief Allocate a ring buffer and wire it into the actor under test.
    void setup_metrics() {
        metric_buf_ =
            std::make_unique<metrics::MpscRingBuffer<metrics::MetricEvent, 64>>();
        eba_->set_metrics_ring_buffer(metric_buf_.get());
    }

    std::unique_ptr<ActorSystem> system_;
    Actor target_;
    EventBasedActor* eba_{nullptr};
    CircuitBreakerTracker* cb_{nullptr};
    FailureRateTracker* ft_{nullptr};
    LifecycleActor* lc_{nullptr};
    std::unique_ptr<metrics::MpscRingBuffer<metrics::MetricEvent, 64>> metric_buf_;
};

// ── HalfOpen + success → Closed, trip_count reset ──────────────────────

TEST_F(CircuitBreakerResultTest, HalfOpenSuccessTransitionsToClosed) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;
    setup_actor(policy);

    // Arrange: circuit is half-open with trip history
    cb_->state = CircuitBreakerState::kHalfOpen;
    cb_->trip_count = 2;
    cb_->half_open_probe_in_flight = true;

    // Act: successful processing
    eba_->record_circuit_breaker_result(true);

    // Assert: transitions to closed, resets trip count
    EXPECT_EQ(cb_->state, CircuitBreakerState::kClosed);
    EXPECT_EQ(cb_->trip_count, 0u);
    EXPECT_FALSE(cb_->half_open_probe_in_flight);
}

// ── HalfOpen + failure → back to Open, trip_count incremented ──────────

TEST_F(CircuitBreakerResultTest, HalfOpenFailureTransitionsBackToOpen) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 2;
    policy.observation_window = milliseconds(1000);
    setup_actor(policy);

    // Arrange: circuit is half-open
    cb_->state = CircuitBreakerState::kHalfOpen;
    cb_->trip_count = 1;
    cb_->half_open_probe_in_flight = true;

    // Produce a failure rate well above threshold=2.
    // rate = 15 failures in 1s window = 15/sec.
    // EMA = 0.1818 * 15 = 2.73 > 2 → trip.
    fill_failure_rate(15.0, 1000);

    // Act: probe fails
    eba_->record_circuit_breaker_result(false);

    // Assert: back to open, trip count incremented
    EXPECT_EQ(cb_->state, CircuitBreakerState::kOpen);
    EXPECT_EQ(cb_->trip_count, 2u);
    EXPECT_FALSE(cb_->half_open_probe_in_flight);
}

// ── Repeated trips ≥ max_circuit_trips → quarantine escalation ─────────

TEST_F(CircuitBreakerResultTest, RepeatedTripsEscalateToQuarantine) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 2;
    policy.observation_window = milliseconds(1000);
    policy.max_circuit_trips = 2; // quarantine after 2nd trip
    setup_lifecycle_actor(policy);

    // Arrange: circuit just tripped to Open for the 2nd time
    cb_->state = CircuitBreakerState::kOpen;
    cb_->trip_count = 2;

    // Produce failure rate well above threshold to trigger trip
    fill_failure_rate(20.0, 1000);

    // Act: failure pushes EMA over threshold, trips again → trip_count=3
    // which exceeds max_circuit_trips=2 → escalated to quarantine
    eba_->record_circuit_breaker_result(false);

    // Assert: actor is now quarantined
    EXPECT_EQ(lc_->state(), LifecycleState::kQuarantined);
    EXPECT_EQ(lc_->quarantine_reason(), QuarantineReason::CircuitBreakerTrip);
}

// ── Closed state — success is a no-op ─────────────────────────────────

TEST_F(CircuitBreakerResultTest, ClosedIgnoresSuccess) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;
    setup_actor(policy);

    // Arrange: normal closed state
    ASSERT_EQ(cb_->state, CircuitBreakerState::kClosed);

    // Act: successful processing
    eba_->record_circuit_breaker_result(true);

    // Assert: no change
    EXPECT_EQ(cb_->state, CircuitBreakerState::kClosed);
    EXPECT_EQ(cb_->trip_count, 0u);
    EXPECT_DOUBLE_EQ(cb_->failure_ema, 0.0);
}

// ── EMA exceeds threshold → circuit trips to Open ──────────────────────

TEST_F(CircuitBreakerResultTest, ClosedTripsOnEmaExceedingThreshold) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;
    policy.observation_window = milliseconds(2000);
    setup_actor(policy);

    // Arrange: normal closed state
    ASSERT_EQ(cb_->state, CircuitBreakerState::kClosed);

    // Produce enough failures for EMA > threshold=5.
    // EMA = α * rate. α = 2/11 ≈ 0.1818.
    // Need EMA > 5 → rate > 5/0.1818 ≈ 27.5.
    // With window=2000ms, need total_failures > 27.5 * 2 = 55.
    // Place 56 failures across 10 buckets.
    for (size_t i = 0; i < FailureRateTracker::kNumBuckets; ++i) {
        ft_->failure_buckets[i] = 6; // 60 total → rate = 30/sec → EMA ≈ 5.45 >
                                     // 5
    }
    ft_->last_bucket_advance = std::chrono::steady_clock::now();
    ft_->bucket_interval_ms = 200;

    // Act: record a failure
    eba_->record_circuit_breaker_result(false);

    // Assert: circuit tripped
    EXPECT_EQ(cb_->state, CircuitBreakerState::kOpen);
    EXPECT_EQ(cb_->trip_count, 1u);
    EXPECT_GT(cb_->failure_ema, 0.0);
}

// ── Threshold of zero disables failure-rate tripping ───────────────────

TEST_F(CircuitBreakerResultTest, NoTripWhenThresholdIsZero) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 0; // disabled
    setup_actor(policy);

    // Arrange: closed state
    ASSERT_EQ(cb_->state, CircuitBreakerState::kClosed);

    // Produce failures even at high rate
    for (size_t i = 0; i < FailureRateTracker::kNumBuckets; ++i) {
        ft_->failure_buckets[i] = 100;
    }
    ft_->last_bucket_advance = std::chrono::steady_clock::now();
    ft_->bucket_interval_ms = 100;

    // Act: record failure
    eba_->record_circuit_breaker_result(false);

    // Assert: circuit stays closed — threshold 0 means no tripping
    EXPECT_EQ(cb_->state, CircuitBreakerState::kClosed);
    EXPECT_EQ(cb_->trip_count, 0u);
}

// ── Disabled quarantine — method returns immediately, no state change ──

TEST_F(CircuitBreakerResultTest, DisabledQuarantineReturnsImmediately) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;
    setup_actor(policy);

    // Manually set circuit to half-open (proves we have access).
    cb_->state = CircuitBreakerState::kHalfOpen;
    cb_->trip_count = 5;

    // Reconfigure to disable quarantine. The internal tracker persists
    // but quarantine_policy_.enabled is now false, so
    // record_circuit_breaker_result() returns immediately.
    QuarantinePolicy disabled;
    disabled.enabled = false;
    eba_->configure_quarantine(disabled);

    // Act: should be a no-op
    eba_->record_circuit_breaker_result(false);

    // Assert: state unchanged — method returned before modifying anything
    EXPECT_EQ(cb_->state, CircuitBreakerState::kHalfOpen);
    EXPECT_EQ(cb_->trip_count, 5u);
}

// ── Circuit state metric emission ──────────────────────────────────────

TEST_F(CircuitBreakerResultTest, EmitsMetricOnCircuitTrip) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;
    policy.observation_window = milliseconds(2000);
    setup_actor(policy);

    setup_metrics();

    // Trigger a trip: produce enough failures to push EMA above threshold.
    for (size_t i = 0; i < FailureRateTracker::kNumBuckets; ++i) {
        ft_->failure_buckets[i] = 6;
    }
    ft_->last_bucket_advance = std::chrono::steady_clock::now();
    ft_->bucket_interval_ms = 200;

    eba_->record_circuit_breaker_result(false);

    // Drain the ring buffer and collect events.
    std::vector<metrics::MetricEvent> events;
    metric_buf_->drain(
        [&](const metrics::MetricEvent& evt) { events.push_back(evt); });

    // We expect at least one kCircuitStateChange event.
    bool found = false;
    for (const auto& evt : events) {
        if (evt.event_type == metrics::MetricEventType::kCircuitStateChange) {
            found = true;
            EXPECT_EQ(evt.code, static_cast<uint8_t>(CircuitBreakerState::kOpen));
            EXPECT_EQ(evt.value_hi, 1u); // trip_count
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected kCircuitStateChange metric event";
}

// ── Metric emitted when circuit closes after successful probe ───────────

TEST_F(CircuitBreakerResultTest, EmitsMetricOnHalfOpenSuccess) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;
    setup_actor(policy);

    setup_metrics();

    // Arrange: circuit is half-open
    cb_->state = CircuitBreakerState::kHalfOpen;
    cb_->trip_count = 2;
    cb_->half_open_probe_in_flight = true;

    // Act: probe succeeds
    eba_->record_circuit_breaker_result(true);

    // Verify metric
    std::vector<metrics::MetricEvent> events;
    metric_buf_->drain(
        [&](const metrics::MetricEvent& evt) { events.push_back(evt); });

    bool found = false;
    for (const auto& evt : events) {
        if (evt.event_type == metrics::MetricEventType::kCircuitStateChange) {
            found = true;
            EXPECT_EQ(evt.code, static_cast<uint8_t>(CircuitBreakerState::kClosed));
            EXPECT_EQ(evt.value_hi, 0u); // trip_count reset
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected kCircuitStateChange for HalfOpen->Closed";
}

// ── Metric emitted when probe fails and circuit re-opens ────────────────

TEST_F(CircuitBreakerResultTest, EmitsMetricOnHalfOpenFailure) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 2;
    policy.observation_window = milliseconds(1000);
    setup_actor(policy);

    setup_metrics();

    // Arrange: circuit is half-open
    cb_->state = CircuitBreakerState::kHalfOpen;
    cb_->trip_count = 1;
    cb_->half_open_probe_in_flight = true;

    // Produce failure rate well above threshold
    fill_failure_rate(15.0, 1000);

    // Act: probe fails
    eba_->record_circuit_breaker_result(false);

    // Verify metric
    std::vector<metrics::MetricEvent> events;
    metric_buf_->drain(
        [&](const metrics::MetricEvent& evt) { events.push_back(evt); });

    bool found = false;
    for (const auto& evt : events) {
        if (evt.event_type == metrics::MetricEventType::kCircuitStateChange) {
            found = true;
            EXPECT_EQ(evt.code, static_cast<uint8_t>(CircuitBreakerState::kOpen));
            EXPECT_EQ(evt.value_hi, 2u); // trip_count incremented
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected kCircuitStateChange for HalfOpen->Open";
}

// ── No metric emitted when quarantine is disabled ──────────────────────

TEST_F(CircuitBreakerResultTest, NoMetricWhenDisabled) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.failure_rate_threshold = 5;
    setup_actor(policy);

    // Install ring buffer, then disable quarantine.
    setup_metrics();

    QuarantinePolicy off;
    off.enabled = false;
    eba_->configure_quarantine(off);

    // Act: this should be a no-op
    eba_->record_circuit_breaker_result(false);

    // Verify: no events emitted
    size_t count = 0;
    metric_buf_->drain([&](const metrics::MetricEvent&) { count++; });
    EXPECT_EQ(count, 0u);
}

// ── Timeout rate: trips circuit when threshold exceeded ────────────────

TEST_F(CircuitBreakerResultTest, TimeoutRateTripsCircuit) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.timeout_rate_threshold = 3; // trip at 3 timeouts/sec
    policy.observation_window = milliseconds(1000);
    setup_actor(policy);

    ASSERT_EQ(cb_->state, CircuitBreakerState::kClosed);

    // Fill timeout buckets: rate = 5 timeouts in 1s = 5/sec > threshold=3
    for (size_t i = 0; i < FailureRateTracker::kNumBuckets; ++i) {
        ft_->timeout_buckets[i] = 1; // 10 total → 10/sec
    }
    ft_->last_bucket_advance = std::chrono::steady_clock::now();
    ft_->bucket_interval_ms = 100;

    eba_->record_circuit_breaker_timeout();

    EXPECT_EQ(cb_->state, CircuitBreakerState::kOpen);
    EXPECT_EQ(cb_->trip_count, 1u);
}

// ── Timeout rate: no trip when threshold is zero ───────────────────────

TEST_F(CircuitBreakerResultTest, TimeoutRateNoTripWhenThresholdZero) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.timeout_rate_threshold = 0; // disabled
    policy.observation_window = milliseconds(1000);
    setup_actor(policy);

    ASSERT_EQ(cb_->state, CircuitBreakerState::kClosed);

    for (size_t i = 0; i < FailureRateTracker::kNumBuckets; ++i) {
        ft_->timeout_buckets[i] = 100;
    }
    ft_->last_bucket_advance = std::chrono::steady_clock::now();
    ft_->bucket_interval_ms = 100;

    eba_->record_circuit_breaker_timeout();

    // No trip — threshold zero disables timeout-based tripping
    EXPECT_EQ(cb_->state, CircuitBreakerState::kClosed);
    EXPECT_EQ(cb_->trip_count, 0u);
}

// ── Timeout rate: no-op when quarantine disabled ───────────────────────

TEST_F(CircuitBreakerResultTest, TimeoutRateNoOpWhenDisabled) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.timeout_rate_threshold = 5;
    setup_actor(policy);

    cb_->state = CircuitBreakerState::kHalfOpen; // should be untouched

    QuarantinePolicy off;
    off.enabled = false;
    eba_->configure_quarantine(off);

    eba_->record_circuit_breaker_timeout();

    // No change — method returned early
    EXPECT_EQ(cb_->state, CircuitBreakerState::kHalfOpen);
}

// ── ActorSystem::record_actor_timeout integration ──────────────────────

TEST_F(CircuitBreakerResultTest, ActorSystemRecordsTimeout) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.timeout_rate_threshold = 3;
    policy.observation_window = milliseconds(1000);
    setup_actor(policy);

    ASSERT_EQ(cb_->state, CircuitBreakerState::kClosed);

    // Fill timeout buckets
    for (size_t i = 0; i < FailureRateTracker::kNumBuckets; ++i) {
        ft_->timeout_buckets[i] = 1;
    }
    ft_->last_bucket_advance = std::chrono::steady_clock::now();
    ft_->bucket_interval_ms = 100;

    system_->record_actor_timeout(target_.id());

    EXPECT_EQ(cb_->state, CircuitBreakerState::kOpen);
    EXPECT_EQ(cb_->trip_count, 1u);
}

// ── ActorSystem::record_actor_timeout handles missing actor gracefully ──

TEST_F(CircuitBreakerResultTest, RecordTimeoutMissingActorGraceful) {
    // Calling record_actor_timeout on a non-existent actor should not crash.
    EXPECT_NO_FATAL_FAILURE(system_->record_actor_timeout(ActorId(99999)));
}

// ── Timeout does not trip when rate is below threshold ─────────────────

TEST_F(CircuitBreakerResultTest, TimeoutBelowThresholdDoesNotTrip) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.timeout_rate_threshold = 10; // high threshold
    policy.observation_window = milliseconds(1000);
    setup_actor(policy);

    ASSERT_EQ(cb_->state, CircuitBreakerState::kClosed);

    // Rate = 5 timeouts/sec < threshold=10
    ft_->timeout_buckets[0] = 5;
    ft_->last_bucket_advance = std::chrono::steady_clock::now();
    ft_->bucket_interval_ms = 100;

    eba_->record_circuit_breaker_timeout();

    EXPECT_EQ(cb_->state, CircuitBreakerState::kClosed);
}

// ── Mailbox pressure: trips circuit when threshold exceeded ────────────

TEST_F(CircuitBreakerResultTest, MailboxPressureTripsCircuit) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.mailbox_pressure_threshold = 0.5f; // trip at 50% pressure
    setup_actor(policy);

    ASSERT_EQ(cb_->state, CircuitBreakerState::kClosed);

    // Fill mailbox above 50% capacity to exceed the threshold.
    // Default capacity is 1024, so 600 messages ≈ 59%.
    auto* mbox = system_->get_mailbox(target_.id());
    ASSERT_NE(mbox, nullptr);

    for (uint32_t i = 0; i < 600; ++i) {
        TypedMessage fill_msg(TypeTag::User, StreamBuffer{1});
        mailbox::MailboxEnvelopeMeta meta;
        mbox->try_push(std::move(fill_msg), meta);
    }

    eba_->check_mailbox_pressure();

    EXPECT_EQ(cb_->state, CircuitBreakerState::kOpen);
    EXPECT_EQ(cb_->trip_count, 1u);
}

TEST_F(CircuitBreakerResultTest, MailboxPressureNoTripWhenThresholdZero) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.mailbox_pressure_threshold = 0.0f; // disabled
    setup_actor(policy);

    ASSERT_EQ(cb_->state, CircuitBreakerState::kClosed);

    eba_->check_mailbox_pressure();

    // No trip — threshold 0.0 disables pressure tripping
    EXPECT_EQ(cb_->state, CircuitBreakerState::kClosed);
}

TEST_F(CircuitBreakerResultTest, MailboxPressureNoOpWhenDisabled) {
    QuarantinePolicy policy;
    policy.enabled = true;
    policy.mailbox_pressure_threshold = 0.5f;
    setup_actor(policy);

    cb_->state = CircuitBreakerState::kHalfOpen;

    QuarantinePolicy off;
    off.enabled = false;
    eba_->configure_quarantine(off);

    eba_->check_mailbox_pressure();

    EXPECT_EQ(cb_->state, CircuitBreakerState::kHalfOpen);
}
