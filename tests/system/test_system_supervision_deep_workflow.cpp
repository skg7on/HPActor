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

// System test: Supervision deep workflow coverage
// Exercises OneForOneSupervisor, AllForOneSupervisor, SupervisorActor,
// SelfSupervisingActor, quarantine escalation, circuit breaker config,
// restart limits with time windows, and Stop directive handling.

#include <gtest/gtest.h>

#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/actor/lifecycle/quarantine_policy.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/supervision/all_for_one_supervisor.hpp>
#include <hpactor/supervision/one_for_one_supervisor.hpp>
#include <hpactor/supervision/supervision.hpp>

#include "scheduler_test_driver.hpp"
#include "system_test_fixture.hpp"

using namespace hpactor;

namespace {

// ── Custom supervisor strategies for testing ─────────────────────────────

class CountingFailureStrategy : public Supervisor {
  public:
    int failure_count = 0;
    int stopped_count = 0;

    SupervisionDirective on_child_failure(const ChildFailure&) override {
        failure_count++;
        return SupervisionDirective::Stop;
    }
    void on_child_stopped(ActorId /*child_id*/) override {
        stopped_count++;
    }
};

class EscalateFailureStrategy : public Supervisor {
  public:
    SupervisionDirective on_child_failure(const ChildFailure&) override {
        return SupervisionDirective::Escalate;
    }
};

class QuarantineFailureStrategy : public Supervisor {
  public:
    SupervisionDirective on_child_failure(const ChildFailure&) override {
        return SupervisionDirective::Quarantine;
    }
};

// Strategy that alternates directives based on call count
class AlternatingDirectiveStrategy : public Supervisor {
  public:
    int call_count = 0;
    SupervisionDirective directive = SupervisionDirective::Restart;

    SupervisionDirective on_child_failure(const ChildFailure&) override {
        call_count++;
        auto result = directive;
        // Alternate between Restart and Stop
        directive = (directive == SupervisionDirective::Restart)
                        ? SupervisionDirective::Stop
                        : SupervisionDirective::Restart;
        return result;
    }
};

/// \brief Testable SelfSupervisingActor that exposes decide_restart for
/// testing.
///
/// The base class declares decide_restart as protected; this subclass
/// re-exports it so tests can drive the restart-policy logic directly.
class TestableSelfSupervisingActor : public SelfSupervisingActor {
  public:
    TestableSelfSupervisingActor(ActorContext* ctx, ActorSystem& sys,
                                 SupervisionPolicy policy = {})
        : SelfSupervisingActor(ctx, sys, policy) {}

    using SelfSupervisingActor::decide_restart;
};

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════════
// Test 1: OneForOneSupervisor restart with policy limits
// ═══════════════════════════════════════════════════════════════════════════

TEST(SupervisionDeep, OneForOneSupervisorWithPolicyLimits) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    SupervisionPolicy policy;
    policy.strategy = SupervisionPolicy::Strategy::OneForOne;
    policy.max_restarts = 3;
    policy.restart_interval = std::chrono::milliseconds{10000};

    auto supervisor = system.spawn<SelfSupervisingActor>(policy);
    ASSERT_TRUE(supervisor);

    auto child = system.spawn<test::FailingActor>();
    ASSERT_TRUE(child);
    auto* fchild = static_cast<test::FailingActor*>(child.get().get());
    fchild->fail_after = 1;

    auto* sup = static_cast<SelfSupervisingActor*>(supervisor.get().get());
    sup->add_child(child);

    // Drain spawn-time notify_ready items
    driver.drain(10);

    // Pin and send one message to trigger failure
    driver.pin_actor_to_worker(child.id(), 0);

    TypedMessage msg(TypeTag(0x2001), StreamBuffer{});
    msg.set_sender_address(supervisor.address());
    fchild->context()->send(child.address(), std::move(msg));

    // Execute the message
    EXPECT_TRUE(driver.run_actor(child.id()));

    // Drain any follow-up work (DownMsg sent to supervisor)
    driver.drain(10);

    // Child should have processed at least the failure-triggering message
    EXPECT_GE(fchild->messages_processed, 1);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 2: AllForOneSupervisor with multiple failures
// ═══════════════════════════════════════════════════════════════════════════

TEST(SupervisionDeep, AllForOneSupervisorMultiFailure) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    AllForOneSupervisor strategy;

    // AllForOne always returns Restart regardless of the child failure
    ChildFailure failure1{ActorId{1}, error(10), SupervisionDirective::Restart};
    auto directive1 = strategy.on_child_failure(failure1);
    EXPECT_EQ(directive1, SupervisionDirective::Restart);

    ChildFailure failure2{ActorId{2}, error(99), SupervisionDirective::Stop};
    auto directive2 = strategy.on_child_failure(failure2);
    EXPECT_EQ(directive2, SupervisionDirective::Restart);

    ChildFailure failure3{ActorId{3}, error(1), SupervisionDirective::Escalate};
    auto directive3 = strategy.on_child_failure(failure3);
    EXPECT_EQ(directive3, SupervisionDirective::Restart);

    // Verify that on_child_stopped is a no-op (inherited from Supervisor base)
    strategy.on_child_stopped(ActorId{42});
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 3: SupervisorActor escalation paths
// ═══════════════════════════════════════════════════════════════════════════

TEST(SupervisionDeep, SupervisorActorEscalationPath) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem sys(cfg);

    EscalateFailureStrategy strategy;
    auto child = sys.spawn<test::CountingActor>();
    ASSERT_TRUE(child);

    std::vector<Actor> children;
    children.push_back(child);

    SupervisorActor supervisor(nullptr, sys, strategy, std::move(children));

    // Simulate an escalation
    ChildFailure failure{child.id(), error(42), SupervisionDirective::Escalate};
    auto directive = strategy.on_child_failure(failure);
    EXPECT_EQ(directive, SupervisionDirective::Escalate);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 4: SelfSupervisingActor remote child tracking
// ═══════════════════════════════════════════════════════════════════════════

TEST(SupervisionDeep, SelfSupervisingActorRemoteChildTracking) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem sys(cfg);

    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, sys, policy);

    auto ep1 = endpoint_ops::parse_endpoint("127.0.0.1:11111");
    auto ep2 = endpoint_ops::parse_endpoint("127.0.0.1:22222");
    auto ep3 = endpoint_ops::parse_endpoint("127.0.0.1:33333");

    ActorAddress addr1(ep1, 0, ActorId{1}, 0);
    ActorAddress addr2(ep2, 0, ActorId{2}, 0);
    ActorAddress addr3(ep3, 0, ActorId{3}, 0);

    ActorProxy proxy1(addr1, static_cast<net::Transport*>(nullptr));
    ActorProxy proxy2(addr2, static_cast<net::Transport*>(nullptr));
    ActorProxy proxy3(addr3, static_cast<net::Transport*>(nullptr));

    ActorRef ref1(std::move(proxy1));
    ActorRef ref2(std::move(proxy2));
    ActorRef ref3(std::move(proxy3));

    supervisor.add_remote_child(ref1);
    supervisor.add_remote_child(ref2);
    supervisor.add_remote_child(ref3);

    EXPECT_TRUE(supervisor.has_remote_child(addr1));
    EXPECT_TRUE(supervisor.has_remote_child(addr2));
    EXPECT_TRUE(supervisor.has_remote_child(addr3));

    auto retrieved1 = supervisor.get_remote_child(addr1);
    EXPECT_EQ(retrieved1.address(), addr1);

    auto retrieved3 = supervisor.get_remote_child(addr3);
    EXPECT_EQ(retrieved3.address(), addr3);

    supervisor.remove_remote_child(addr2);
    EXPECT_FALSE(supervisor.has_remote_child(addr2));
    EXPECT_TRUE(supervisor.has_remote_child(addr1));
    EXPECT_TRUE(supervisor.has_remote_child(addr3));

    supervisor.remove_remote_child(addr1);
    supervisor.remove_remote_child(addr3);
    EXPECT_FALSE(supervisor.has_remote_child(addr1));
    EXPECT_FALSE(supervisor.has_remote_child(addr3));

    EXPECT_EQ(supervisor.remote_children().size(), 0u);

    ActorAddress unknown(ep1, 0, ActorId{999}, 0);
    EXPECT_FALSE(supervisor.has_remote_child(unknown));

    // get_remote_child on unknown address returns an ActorRef wrapping an
    // invalid address — ActorAddress has an explicit operator bool.
    auto unknown_ref = supervisor.get_remote_child(unknown);
    EXPECT_FALSE(static_cast<bool>(unknown_ref.address()));
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 5: Quarantine escalation from supervision
// ═══════════════════════════════════════════════════════════════════════════

TEST(SupervisionDeep, QuarantineEscalationFromSupervision) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem sys(cfg);

    // Policy with quarantine enabled and escalate_on_max_restarts
    SupervisionPolicy policy;
    policy.max_restarts = 2;
    policy.restart_interval = std::chrono::milliseconds{10000};
    policy.quarantine.enabled = true;
    policy.quarantine.escalate_on_max_restarts = true;

    TestableSelfSupervisingActor supervisor(nullptr, sys, policy);

    auto result1 = supervisor.decide_restart(ActorId{100}, error(1));
    EXPECT_EQ(result1, SupervisionDirective::Restart);

    auto result2 = supervisor.decide_restart(ActorId{100}, error(2));
    EXPECT_EQ(result2, SupervisionDirective::Restart);

    // Third failure should trigger quarantine (max_restarts = 2)
    auto result3 = supervisor.decide_restart(ActorId{100}, error(3));
    EXPECT_EQ(result3, SupervisionDirective::Quarantine);

    // Policy with quarantine enabled but escalate_on_max_restarts = false
    SupervisionPolicy policy_no_escalate;
    policy_no_escalate.max_restarts = 1;
    policy_no_escalate.restart_interval = std::chrono::milliseconds{10000};
    policy_no_escalate.quarantine.enabled = true;
    policy_no_escalate.quarantine.escalate_on_max_restarts = false;

    TestableSelfSupervisingActor supervisor2(nullptr, sys, policy_no_escalate);

    auto r1 = supervisor2.decide_restart(ActorId{200}, error(1));
    EXPECT_EQ(r1, SupervisionDirective::Restart);

    // Second failure should trigger Stop (quarantine escalation disabled)
    auto r2 = supervisor2.decide_restart(ActorId{200}, error(2));
    EXPECT_EQ(r2, SupervisionDirective::Stop);

    // Quarantine directive through supervisor strategy
    QuarantineFailureStrategy q_strategy;
    ChildFailure q_failure{ActorId{1}, error(55), SupervisionDirective::Quarantine};
    auto q_directive = q_strategy.on_child_failure(q_failure);
    EXPECT_EQ(q_directive, SupervisionDirective::Quarantine);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 6: Circuit breaker / QuarantinePolicy configuration
// ═══════════════════════════════════════════════════════════════════════════

TEST(SupervisionDeep, CircuitBreakerTransitionConfiguration) {
    QuarantinePolicy default_policy;

    EXPECT_FALSE(default_policy.enabled);
    EXPECT_TRUE(default_policy.escalate_on_max_restarts);

    EXPECT_EQ(default_policy.failure_rate_threshold, 0u);
    EXPECT_EQ(default_policy.timeout_rate_threshold, 0u);
    EXPECT_EQ(default_policy.mailbox_pressure_threshold, 0.0f);

    EXPECT_EQ(default_policy.cooldown_period, std::chrono::milliseconds{30000});
    EXPECT_EQ(default_policy.observation_window, std::chrono::milliseconds{10000});
    EXPECT_EQ(default_policy.max_circuit_trips, 3u);

    // Configure a full circuit breaker policy
    QuarantinePolicy cb_policy;
    cb_policy.enabled = true;
    cb_policy.failure_rate_threshold = 10;
    cb_policy.timeout_rate_threshold = 5;
    cb_policy.mailbox_pressure_threshold = 0.8f;
    cb_policy.cooldown_period = std::chrono::milliseconds{15000};
    cb_policy.observation_window = std::chrono::milliseconds{5000};
    cb_policy.max_circuit_trips = 5;

    EXPECT_TRUE(cb_policy.enabled);
    EXPECT_EQ(cb_policy.failure_rate_threshold, 10u);
    EXPECT_EQ(cb_policy.timeout_rate_threshold, 5u);
    EXPECT_FLOAT_EQ(cb_policy.mailbox_pressure_threshold, 0.8f);
    EXPECT_EQ(cb_policy.cooldown_period, std::chrono::milliseconds{15000});
    EXPECT_EQ(cb_policy.observation_window, std::chrono::milliseconds{5000});
    EXPECT_EQ(cb_policy.max_circuit_trips, 5u);

    // Verify integration with supervision policy
    SupervisionPolicy sup_policy;
    sup_policy.quarantine = cb_policy;

    EXPECT_TRUE(sup_policy.quarantine.enabled);
    EXPECT_EQ(sup_policy.quarantine.failure_rate_threshold, 10u);
    EXPECT_EQ(sup_policy.quarantine.timeout_rate_threshold, 5u);
    EXPECT_EQ(sup_policy.quarantine.max_circuit_trips, 5u);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 7: Supervisor with max_restarts and time window
// ═══════════════════════════════════════════════════════════════════════════

TEST(SupervisionDeep, MaxRestartsWithTimeWindow) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem sys(cfg);

    SupervisionPolicy policy;
    policy.max_restarts = 3;
    policy.restart_interval = std::chrono::milliseconds{60000};

    TestableSelfSupervisingActor supervisor(nullptr, sys, policy);

    auto d1 = supervisor.decide_restart(ActorId{42}, error(10));
    EXPECT_EQ(d1, SupervisionDirective::Restart);

    auto d2 = supervisor.decide_restart(ActorId{42}, error(11));
    EXPECT_EQ(d2, SupervisionDirective::Restart);

    auto d3 = supervisor.decide_restart(ActorId{42}, error(12));
    EXPECT_EQ(d3, SupervisionDirective::Restart);

    // Fourth restart should hit limit
    auto d4 = supervisor.decide_restart(ActorId{42}, error(13));
    EXPECT_EQ(d4, SupervisionDirective::Stop);

    // Different child should have independent restart count
    auto d5 = supervisor.decide_restart(ActorId{99}, error(1));
    EXPECT_EQ(d5, SupervisionDirective::Restart);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 8: Stop directive handling
// ═══════════════════════════════════════════════════════════════════════════

TEST(SupervisionDeep, StopDirectiveHandling) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem sys(cfg);

    CountingFailureStrategy strategy;

    auto child1 = sys.spawn<test::CountingActor>();
    auto child2 = sys.spawn<test::CountingActor>();
    ASSERT_TRUE(child1);
    ASSERT_TRUE(child2);

    ActorId id1 = child1.id();
    ActorId id2 = child2.id();

    std::vector<Actor> children;
    children.push_back(child1);
    children.push_back(child2);

    SupervisorActor supervisor(nullptr, sys, strategy, std::move(children));

    ChildFailure failure1{id1, error(42), SupervisionDirective::Stop};
    auto directive1 = strategy.on_child_failure(failure1);
    EXPECT_EQ(directive1, SupervisionDirective::Stop);
    EXPECT_EQ(strategy.failure_count, 1);

    strategy.on_child_stopped(id1);
    EXPECT_EQ(strategy.stopped_count, 1);

    ChildFailure failure2{id2, error(99), SupervisionDirective::Stop};
    auto directive2 = strategy.on_child_failure(failure2);
    EXPECT_EQ(directive2, SupervisionDirective::Stop);
    EXPECT_EQ(strategy.failure_count, 2);

    strategy.on_child_stopped(id2);
    EXPECT_EQ(strategy.stopped_count, 2);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 9: Alternating supervision directives
// ═══════════════════════════════════════════════════════════════════════════

TEST(SupervisionDeep, AlternatingDirectivesStrategy) {
    AlternatingDirectiveStrategy strategy;
    EXPECT_EQ(strategy.call_count, 0);

    auto d1 = strategy.on_child_failure(
        ChildFailure{ActorId{1}, error(1), SupervisionDirective::Restart});
    EXPECT_EQ(d1, SupervisionDirective::Restart);
    EXPECT_EQ(strategy.call_count, 1);

    auto d2 = strategy.on_child_failure(
        ChildFailure{ActorId{1}, error(2), SupervisionDirective::Restart});
    EXPECT_EQ(d2, SupervisionDirective::Stop);
    EXPECT_EQ(strategy.call_count, 2);

    auto d3 = strategy.on_child_failure(
        ChildFailure{ActorId{1}, error(3), SupervisionDirective::Restart});
    EXPECT_EQ(d3, SupervisionDirective::Restart);
    EXPECT_EQ(strategy.call_count, 3);

    auto d4 = strategy.on_child_failure(
        ChildFailure{ActorId{1}, error(4), SupervisionDirective::Restart});
    EXPECT_EQ(d4, SupervisionDirective::Stop);
    EXPECT_EQ(strategy.call_count, 4);
}

// ═══════════════════════════════════════════════════════════════════════════
// Test 10: SelfSupervisingActor local children add/remove
// ═══════════════════════════════════════════════════════════════════════════

TEST(SupervisionDeep, SelfSupervisingActorLocalChildrenAddRemove) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem sys(cfg);

    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, sys, policy);

    auto child1 = sys.spawn<test::CountingActor>();
    auto child2 = sys.spawn<test::CountingActor>();
    auto child3 = sys.spawn<test::FailingActor>();

    supervisor.add_child(child1);
    supervisor.add_child(child2);
    supervisor.add_child(child3);

    supervisor.remove_child(child2);
    supervisor.remove_child(child1);
    supervisor.remove_child(child3);

    // Remove a child that was never added (should be safe)
    auto unadded = sys.spawn<test::CountingActor>();
    supervisor.remove_child(unadded);
    SUCCEED();
}
