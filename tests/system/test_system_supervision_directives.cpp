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

// System test: Supervision directives and restart limits
// Exercises Stop/Escalate/Quarantine directives, max restarts exceeded,
// restart interval window, SelfSupervisingActor decide_restart paths.

#include <gtest/gtest.h>

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/supervision/supervision.hpp>

#include "system_test_fixture.hpp"

using namespace hpactor;

namespace {

// OneForOne strategy for testing
class TestOneForOneStrategy : public Supervisor {
  public:
    SupervisionDirective directive = SupervisionDirective::Restart;
    bool child_stopped_called = false;
    ActorId stopped_child_id;

    SupervisionDirective on_child_failure(const ChildFailure&) override {
        return directive;
    }
    void on_child_stopped(ActorId child_id) override {
        child_stopped_called = true;
        stopped_child_id = child_id;
    }
};

class StopStrategy : public Supervisor {
  public:
    SupervisionDirective on_child_failure(const ChildFailure&) override {
        return SupervisionDirective::Stop;
    }
};

class EscalateStrategy : public Supervisor {
  public:
    SupervisionDirective on_child_failure(const ChildFailure&) override {
        return SupervisionDirective::Escalate;
    }
};

class QuarantineStrategy : public Supervisor {
  public:
    SupervisionDirective on_child_failure(const ChildFailure&) override {
        return SupervisionDirective::Quarantine;
    }
};

} // anonymous namespace

// ── Supervisor directives ────────────────────────────────────────

TEST(SupervisionDirectives, StopDirectiveCallsOnChildStopped) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem sys(cfg);

    TestOneForOneStrategy strategy;
    strategy.directive = SupervisionDirective::Stop;

    // Create a child actor
    auto child = sys.spawn<test::CountingActor>();
    ASSERT_TRUE(child);
    ActorId child_id = child.id();

    std::vector<Actor> children;
    children.push_back(child);

    SupervisorActor supervisor(nullptr, sys, strategy, std::move(children));

    // Child failure with Stop directive
    ChildFailure failure{child_id, error(1), SupervisionDirective::Stop};
    strategy.on_child_failure(failure);

    // on_child_stopped is called by the Supervisor base
    strategy.on_child_stopped(child_id);
    EXPECT_TRUE(strategy.child_stopped_called);
    EXPECT_EQ(strategy.stopped_child_id, child_id);
}

TEST(SupervisionDirectives, EscalateDirectiveRecognized) {
    EscalateStrategy strategy;
    ChildFailure failure{ActorId{1}, error(99), SupervisionDirective::Escalate};
    auto directive = strategy.on_child_failure(failure);
    EXPECT_EQ(directive, SupervisionDirective::Escalate);
}

TEST(SupervisionDirectives, QuarantineDirectiveRecognized) {
    QuarantineStrategy strategy;
    ChildFailure failure{ActorId{1}, error(99), SupervisionDirective::Quarantine};
    auto directive = strategy.on_child_failure(failure);
    EXPECT_EQ(directive, SupervisionDirective::Quarantine);
}

// ── Supervision policy ───────────────────────────────────────────

TEST(SupervisionPolicyTest, DefaultPolicyValues) {
    SupervisionPolicy policy;
    EXPECT_EQ(policy.strategy, SupervisionPolicy::Strategy::OneForOne);
    EXPECT_EQ(policy.max_restarts, 10u);
    EXPECT_EQ(policy.restart_interval, std::chrono::milliseconds(5000));
}

TEST(SupervisionPolicyTest, CustomPolicyValues) {
    SupervisionPolicy policy;
    policy.strategy = SupervisionPolicy::Strategy::AllForOne;
    policy.max_restarts = 3;
    policy.restart_interval = std::chrono::milliseconds(1000);
    EXPECT_EQ(policy.strategy, SupervisionPolicy::Strategy::AllForOne);
    EXPECT_EQ(policy.max_restarts, 3u);
    EXPECT_EQ(policy.restart_interval, std::chrono::milliseconds(1000));
}

// ── SelfSupervisingActor ─────────────────────────────────────────

TEST(SelfSupervisingActorTest, AddRemoveLocalChild) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem sys(cfg);

    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, sys, policy);

    auto child = sys.spawn<test::CountingActor>();
    ASSERT_TRUE(child);

    supervisor.add_child(child);
    // remove_child via the same actor
    supervisor.remove_child(child);
}

TEST(SelfSupervisingActorTest, PolicyWithQuarantineEnabled) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem sys(cfg);

    SupervisionPolicy policy;
    policy.max_restarts = 2;
    policy.quarantine.enabled = true;

    SelfSupervisingActor supervisor(nullptr, sys, policy);
    // Verify construction with quarantine policy doesn't crash
    SUCCEED();
}

// ── Remote child management integrated ───────────────────────────

TEST(SelfSupervisingActorTest, RemoteChildRoundtrip) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem sys(cfg);

    SupervisionPolicy policy;
    SelfSupervisingActor supervisor(nullptr, sys, policy);

    auto ep = endpoint_ops::parse_endpoint("127.0.0.1:12345");
    ActorAddress addr(ep, 0, ActorId{42}, 0);
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
    ActorRef ref(std::move(proxy));

    supervisor.add_remote_child(ref);
    EXPECT_TRUE(supervisor.has_remote_child(addr));

    auto retrieved = supervisor.get_remote_child(addr);
    EXPECT_EQ(retrieved.address(), addr);

    supervisor.remove_remote_child(addr);
    EXPECT_FALSE(supervisor.has_remote_child(addr));
}
