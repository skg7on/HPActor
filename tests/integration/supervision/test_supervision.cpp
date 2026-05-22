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

#include <hpactor/core/actor_system.hpp>
#include <hpactor/supervision/all_for_one_supervisor.hpp>
#include <hpactor/supervision/one_for_one_supervisor.hpp>
#include <hpactor/supervision/supervision.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

// Fixture for tests that need an ActorSystem
class SupervisionIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Config cfg;
        cfg.scheduler_threads = 0;
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

// Pure unit tests — no ActorSystem needed

TEST(SupervisionTest, DirectiveValues) {
    EXPECT_EQ(static_cast<int>(SupervisionDirective::Restart), 0);
    EXPECT_EQ(static_cast<int>(SupervisionDirective::Stop), 1);
    EXPECT_EQ(static_cast<int>(SupervisionDirective::Escalate), 2);
}

TEST(SupervisionTest, ChildFailureStruct) {
    ActorId id(42);
    ChildFailure failure{id, error{1, "test error"}, SupervisionDirective::Restart};
    EXPECT_EQ(failure.child_id.value(), 42u);
    EXPECT_EQ(failure.reason.code(), 1);
    EXPECT_EQ(failure.directive, SupervisionDirective::Restart);
}

TEST(SupervisionTest, PolicyDefault) {
    SupervisionPolicy policy;
    EXPECT_EQ(policy.strategy, SupervisionPolicy::Strategy::OneForOne);
    EXPECT_EQ(policy.max_restarts, 10);
    EXPECT_EQ(policy.restart_interval.count(), 5000);
}

TEST(SupervisionTest, PolicyCustom) {
    SupervisionPolicy policy;
    policy.strategy = SupervisionPolicy::Strategy::AllForOne;
    policy.max_restarts = 5;
    policy.restart_interval = std::chrono::milliseconds(1000);
    EXPECT_EQ(policy.strategy, SupervisionPolicy::Strategy::AllForOne);
    EXPECT_EQ(policy.max_restarts, 5);
    EXPECT_EQ(policy.restart_interval.count(), 1000);
}

TEST(SupervisionTest, OneForOnePassesDirective) {
    SupervisionPolicy policy;
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
}

TEST(SupervisionTest, AllForOneAlwaysRestart) {
    SupervisionPolicy policy;
    AllForOneSupervisor sup(policy);
    ChildFailure failure;
    failure.child_id = ActorId{1};
    failure.reason = error(0);
    failure.directive = SupervisionDirective::Stop;
    EXPECT_EQ(sup.on_child_failure(failure), SupervisionDirective::Restart);
}

// ActorSystem-dependent tests

TEST_F(SupervisionIntegrationTest, SupervisorActorConstruct) {
    OneForOneSupervisor strategy(SupervisionPolicy{});
    std::vector<Actor> children;
    SupervisorActor actor(nullptr, *system_, strategy, std::move(children));
    (void)actor;
    SUCCEED();
}

TEST_F(SupervisionIntegrationTest, SelfSupervisingConstruct) {
    SupervisionPolicy policy;
    SelfSupervisingActor actor(nullptr, *system_, policy);
    (void)actor;
    SUCCEED();
}
