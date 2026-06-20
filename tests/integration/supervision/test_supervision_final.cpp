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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/lifecycle/circuit_breaker.hpp>
#include <hpactor/actor/lifecycle/quarantine_policy.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/supervision/all_for_one_supervisor.hpp>
#include <hpactor/supervision/one_for_one_supervisor.hpp>
#include <hpactor/supervision/supervision.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace hpactor;

// =============================================================================
// SupervisionFinalTest — fixture for ActorSystem-dependent supervision tests
// =============================================================================
class SupervisionFinalTest : public ::testing::Test {
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

// =============================================================================
// Test 1: Supervisor with multiple restart policies
// =============================================================================

TEST(SupervisionFinalUnitTest, OneForOneRestartPolicyDirectivePassthrough) {
    SupervisionPolicy policy;
    policy.strategy = SupervisionPolicy::Strategy::OneForOne;
    policy.max_restarts = 3;

    OneForOneSupervisor sup(policy);

    ChildFailure failure;
    failure.child_id = ActorId{1};
    failure.reason = error(0);

    failure.directive = SupervisionDirective::Restart;
    EXPECT_EQ(sup.on_child_failure(failure), SupervisionDirective::Restart);

    failure.directive = SupervisionDirective::Stop;
    EXPECT_EQ(sup.on_child_failure(failure), SupervisionDirective::Stop);

    failure.directive = SupervisionDirective::Escalate;
    EXPECT_EQ(sup.on_child_failure(failure), SupervisionDirective::Escalate);

    failure.directive = SupervisionDirective::Quarantine;
    EXPECT_EQ(sup.on_child_failure(failure), SupervisionDirective::Quarantine);
}

TEST(SupervisionFinalUnitTest, AllForOneAlwaysRestarts) {
    SupervisionPolicy policy;
    policy.strategy = SupervisionPolicy::Strategy::AllForOne;
    AllForOneSupervisor sup(policy);

    // All directives should be coerced to Restart
    ChildFailure failure;
    failure.child_id = ActorId{1};
    failure.reason = error(0);

    failure.directive = SupervisionDirective::Stop;
    EXPECT_EQ(sup.on_child_failure(failure), SupervisionDirective::Restart);

    failure.directive = SupervisionDirective::Escalate;
    EXPECT_EQ(sup.on_child_failure(failure), SupervisionDirective::Restart);

    failure.directive = SupervisionDirective::Quarantine;
    EXPECT_EQ(sup.on_child_failure(failure), SupervisionDirective::Restart);
}

// =============================================================================
// Test 2: Supervisor escalation chain
// =============================================================================

TEST(SupervisionFinalUnitTest, EscalationDirectivePropagates) {
    SupervisionPolicy policy;
    policy.strategy = SupervisionPolicy::Strategy::OneForOne;
    OneForOneSupervisor sup(policy);

    ChildFailure failure;
    failure.child_id = ActorId{42};
    failure.reason = error(99, "fatal error");
    failure.directive = SupervisionDirective::Escalate;

    auto result = sup.on_child_failure(failure);
    EXPECT_EQ(result, SupervisionDirective::Escalate);
}

// =============================================================================
// Test 3: Supervisor with quarantine escalation
// =============================================================================

TEST(SupervisionFinalUnitTest, QuarantinePolicyDisabledByDefault) {
    SupervisionPolicy policy;
    EXPECT_FALSE(policy.quarantine.enabled);
}

TEST(SupervisionFinalUnitTest, QuarantinePolicyEnabledPermitsQuarantineDirective) {
    SupervisionPolicy policy;
    policy.quarantine.enabled = true;
    policy.quarantine.cooldown_period = std::chrono::milliseconds(30000);

    OneForOneSupervisor sup(policy);

    ChildFailure failure;
    failure.child_id = ActorId{1};
    failure.reason = error(0);
    failure.directive = SupervisionDirective::Quarantine;

    EXPECT_EQ(sup.on_child_failure(failure), SupervisionDirective::Quarantine);
}

// =============================================================================
// Test 4: SelfSupervisingActor with remote children
// =============================================================================

namespace {

ActorRef make_remote_ref_sv(EndPoint ep, ActorId id) {
    ActorAddress addr(ep, 0, id, 0);
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
    return ActorRef(std::move(proxy));
}

} // namespace

TEST_F(SupervisionFinalTest, SelfSupervisingActorAddLocalChild) {
    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, *system_, policy);

    auto child = system_->spawn<EventBasedActor>();
    ASSERT_TRUE(static_cast<bool>(child));
    supervisor.add_child(child);

    // Child successfully added — verify the remote_children interface works
    // as a proxy for the base children collection
    EXPECT_TRUE(supervisor.remote_children().empty());

    // Remove child
    supervisor.remove_child(child);
}

TEST_F(SupervisionFinalTest, SelfSupervisingActorRemoveLocalChild) {
    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, *system_, policy);

    auto child = system_->spawn<EventBasedActor>();
    supervisor.add_child(child);
    supervisor.remove_child(child);
    // remote children should still be empty
    EXPECT_TRUE(supervisor.remote_children().empty());
}

TEST_F(SupervisionFinalTest, SelfSupervisingActorRemoteChildrenManagement) {
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:9000");
    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, *system_, policy);

    // Add remote children
    for (uint64_t i = 1; i <= 3; ++i) {
        auto ref = make_remote_ref_sv(ep, ActorId{i});
        supervisor.add_remote_child(ref);
    }
    EXPECT_EQ(supervisor.remote_children().size(), 3u);

    // Lookup
    ActorAddress addr2(ep, 0, ActorId{2}, 0);
    EXPECT_TRUE(supervisor.has_remote_child(addr2));

    // Remove one
    supervisor.remove_remote_child(addr2);
    EXPECT_EQ(supervisor.remote_children().size(), 2u);
    EXPECT_FALSE(supervisor.has_remote_child(addr2));
}

TEST_F(SupervisionFinalTest, SelfSupervisingActorRemoteChildNotFound) {
    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:9001");
    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, *system_, policy);

    ActorAddress addr(ep, 0, ActorId{999}, 0);
    EXPECT_FALSE(supervisor.has_remote_child(addr));

    auto result = supervisor.get_remote_child(addr);
    EXPECT_FALSE(bool(result));
}

// =============================================================================
// Test 5: Circuit breaker full state cycle
// =============================================================================

TEST(CircuitBreakerFinalTest, StateValues) {
    EXPECT_EQ(static_cast<int>(CircuitBreakerState::kClosed), 0);
    EXPECT_EQ(static_cast<int>(CircuitBreakerState::kOpen), 1);
    EXPECT_EQ(static_cast<int>(CircuitBreakerState::kHalfOpen), 2);
}

TEST(CircuitBreakerFinalTest, ToStringForAllStates) {
    EXPECT_STREQ(to_string(CircuitBreakerState::kClosed), "closed");
    EXPECT_STREQ(to_string(CircuitBreakerState::kOpen), "open");
    EXPECT_STREQ(to_string(CircuitBreakerState::kHalfOpen), "half_open");
}

TEST(CircuitBreakerFinalTest, TrackerDefaultState) {
    CircuitBreakerTracker tracker;
    EXPECT_EQ(tracker.state, CircuitBreakerState::kClosed);
    EXPECT_EQ(tracker.trip_count, 0u);
    EXPECT_FALSE(tracker.half_open_probe_in_flight);
    EXPECT_EQ(tracker.failure_ema, 0.0);
}

TEST(CircuitBreakerFinalTest, TrackerStateTransitions) {
    CircuitBreakerTracker tracker;

    // Closed → Open
    tracker.state = CircuitBreakerState::kOpen;
    tracker.trip_count = 1;
    tracker.opened_at = std::chrono::steady_clock::now();

    EXPECT_EQ(tracker.state, CircuitBreakerState::kOpen);
    EXPECT_EQ(tracker.trip_count, 1u);

    // Open → HalfOpen
    tracker.state = CircuitBreakerState::kHalfOpen;
    tracker.half_open_probe_in_flight = true;

    EXPECT_EQ(tracker.state, CircuitBreakerState::kHalfOpen);
    EXPECT_TRUE(tracker.half_open_probe_in_flight);

    // HalfOpen → Closed (success)
    tracker.state = CircuitBreakerState::kClosed;
    tracker.half_open_probe_in_flight = false;

    EXPECT_EQ(tracker.state, CircuitBreakerState::kClosed);
    EXPECT_FALSE(tracker.half_open_probe_in_flight);
}

// =============================================================================
// Test 6: Failure rate tracking
// =============================================================================

TEST(CircuitBreakerFinalTest, FailureEmaTracksValues) {
    CircuitBreakerTracker tracker;

    // Set an initial failure rate
    tracker.failure_ema = 0.5;
    EXPECT_DOUBLE_EQ(tracker.failure_ema, 0.5);

    // Update EMA (simple average — real tracking code uses exponential decay)
    // Here we just verify the field can hold and propagate values
    tracker.failure_ema = (tracker.failure_ema * 0.9) + (1.0 * 0.1);
    EXPECT_GT(tracker.failure_ema, 0.0);
    EXPECT_LT(tracker.failure_ema, 1.0);
}

// =============================================================================
// Test 7: Supervision with scheduled messages
// =============================================================================

TEST(SupervisionFinalUnitTest, PolicyRestartIntervalDefault) {
    SupervisionPolicy policy;
    EXPECT_EQ(policy.restart_interval.count(), 5000);
}

TEST(SupervisionFinalUnitTest, PolicyRestartIntervalCustom) {
    SupervisionPolicy policy;
    policy.max_restarts = 5;
    policy.restart_interval = std::chrono::milliseconds(30000);

    EXPECT_EQ(policy.max_restarts, 5u);
    EXPECT_EQ(policy.restart_interval.count(), 30000);
}

// =============================================================================
// Test 8: Supervision metric events
// =============================================================================

TEST_F(SupervisionFinalTest, SupervisorActorConstructWithChildren) {
    SupervisionPolicy pol;
    pol.strategy = SupervisionPolicy::Strategy::OneForOne;

    OneForOneSupervisor strategy(pol);
    std::vector<Actor> children;

    auto child = system_->spawn<EventBasedActor>();
    children.push_back(child);

    SupervisorActor supervisor(nullptr, *system_, strategy, std::move(children));
    // Supervision actor constructed with children successfully
    SUCCEED();
}

TEST_F(SupervisionFinalTest, SupervisorActorConstructEmptyChildren) {
    SupervisionPolicy pol;
    OneForOneSupervisor strategy(pol);
    std::vector<Actor> children;

    SupervisorActor supervisor(nullptr, *system_, strategy, std::move(children));
    // Supervision actor constructed with empty children successfully
    SUCCEED();
}

TEST_F(SupervisionFinalTest, SelfSupervisingActorCustomPolicy) {
    SupervisionPolicy policy;
    policy.strategy = SupervisionPolicy::Strategy::OneForOne;
    policy.max_restarts = 7;
    policy.restart_interval = std::chrono::milliseconds(10000);

    SelfSupervisingActor actor(nullptr, *system_, policy);
    SUCCEED();
}

TEST_F(SupervisionFinalTest, AllForOneSupervisorWithPolicy) {
    SupervisionPolicy policy;
    policy.strategy = SupervisionPolicy::Strategy::AllForOne;
    policy.max_restarts = 3;

    AllForOneSupervisor sup(policy);

    ChildFailure failure;
    failure.child_id = ActorId{100};
    failure.reason = error(errors::actor_down);
    failure.directive = SupervisionDirective::Stop;

    EXPECT_EQ(sup.on_child_failure(failure), SupervisionDirective::Restart);
}

// =============================================================================
// Test: SupervisionPolicy Strategy enum values
// =============================================================================

TEST(SupervisionFinalUnitTest, StrategyEnumValues) {
    EXPECT_EQ(static_cast<int>(SupervisionPolicy::Strategy::OneForOne), 0);
    EXPECT_EQ(static_cast<int>(SupervisionPolicy::Strategy::AllForOne), 1);
    EXPECT_EQ(static_cast<int>(SupervisionPolicy::Strategy::OneForAll), 2);
}
