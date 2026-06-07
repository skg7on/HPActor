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

#include <gtest/gtest.h>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/types/types.hpp>

#include <string>

using namespace hpactor;

// Test harness: LifecycleActor with tracking
class TestLifecycleActor : public LifecycleActor {
  public:
    int start_calls = 0;
    int drain_calls = 0;
    int stop_calls = 0;
    int deactivate_calls = 0;
    int fail_calls = 0;
    int recover_calls = 0;
    int restart_calls = 0;

    void on_start() override {
        start_calls++;
    }
    void on_drain() override {
        drain_calls++;
    }
    void on_stop() override {
        stop_calls++;
    }
    void on_deactivate() override {
        deactivate_calls++;
    }
    void on_fail(error) override {
        fail_calls++;
    }
    void on_recover() override {
        recover_calls++;
    }
    void on_restart() override {
        restart_calls++;
    }
};

TEST(LifecycleStateTest, DefaultStateIsStarting) {
    TestLifecycleActor a;
    EXPECT_EQ(a.state(), LifecycleState::kStarting);
    EXPECT_EQ(std::string(a.state_string()), "starting");
}

TEST(LifecycleStateTest, StartingToActive) {
    TestLifecycleActor a;
    bool ok = a.transition(LifecycleState::kActive);
    EXPECT_TRUE(ok);
    EXPECT_EQ(a.state(), LifecycleState::kActive);
    EXPECT_EQ(a.start_calls, 1);
}

TEST(LifecycleStateTest, ActiveToStartingIllegal) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    bool ok = a.transition(LifecycleState::kStarting);
    EXPECT_FALSE(ok);
    EXPECT_EQ(a.state(), LifecycleState::kActive);
}

TEST(LifecycleStateTest, ActiveToRecoveringIllegal) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    bool ok = a.transition(LifecycleState::kRecovering);
    EXPECT_FALSE(ok);
    EXPECT_EQ(a.state(), LifecycleState::kActive);
}

TEST(LifecycleStateTest, FullHappyPath) {
    TestLifecycleActor a;
    EXPECT_TRUE(a.transition(LifecycleState::kActive));
    EXPECT_EQ(a.start_calls, 1);
    EXPECT_TRUE(a.transition(LifecycleState::kDraining));
    EXPECT_EQ(a.drain_calls, 1);
    EXPECT_TRUE(a.transition(LifecycleState::kStopping));
    EXPECT_EQ(a.stop_calls, 1);
    EXPECT_TRUE(a.transition(LifecycleState::kStopped));
    EXPECT_EQ(a.deactivate_calls, 1);
    EXPECT_EQ(a.state(), LifecycleState::kStopped);
}

TEST(LifecycleStateTest, FailureRestartPath) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    EXPECT_TRUE(a.transition(LifecycleState::kFailed));
    EXPECT_EQ(a.fail_calls, 1);
    EXPECT_EQ(a.state(), LifecycleState::kFailed);
    a.bump_incarnation();
    EXPECT_TRUE(a.transition(LifecycleState::kStarting));
    EXPECT_EQ(a.restart_calls, 1);
    EXPECT_TRUE(a.transition(LifecycleState::kActive));
    EXPECT_EQ(a.start_calls, 2);
}

TEST(LifecycleStateTest, RecoveryPath) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    a.transition(LifecycleState::kFailed);
    EXPECT_TRUE(a.transition(LifecycleState::kRecovering));
    EXPECT_EQ(a.recover_calls, 1);
    EXPECT_TRUE(a.transition(LifecycleState::kActive));
    EXPECT_EQ(a.start_calls, 2);
}

TEST(LifecycleStateTest, StateString) {
    TestLifecycleActor a;
    EXPECT_EQ(std::string(a.state_string()), "starting");
    a.transition(LifecycleState::kActive);
    EXPECT_EQ(std::string(a.state_string()), "active");
    a.transition(LifecycleState::kDraining);
    EXPECT_EQ(std::string(a.state_string()), "draining");
    a.transition(LifecycleState::kStopping);
    EXPECT_EQ(std::string(a.state_string()), "stopping");
    a.transition(LifecycleState::kStopped);
    EXPECT_EQ(std::string(a.state_string()), "stopped");
}

TEST(LifecycleStateTest, AcceptsUserMsgs) {
    TestLifecycleActor a;
    EXPECT_FALSE(a.accepts_user_msgs());
    a.transition(LifecycleState::kActive);
    EXPECT_TRUE(a.accepts_user_msgs());
    a.transition(LifecycleState::kDraining);
    EXPECT_FALSE(a.accepts_user_msgs());
}

TEST(LifecycleStateTest, AcceptsSystemMsgs) {
    TestLifecycleActor a;
    EXPECT_TRUE(a.accepts_system_msgs());
    a.transition(LifecycleState::kActive);
    EXPECT_TRUE(a.accepts_system_msgs());
    a.transition(LifecycleState::kDraining);
    EXPECT_TRUE(a.accepts_system_msgs());
    a.transition(LifecycleState::kStopping);
    EXPECT_TRUE(a.accepts_system_msgs());
    a.transition(LifecycleState::kStopped);
    EXPECT_FALSE(a.accepts_system_msgs());
}

TEST(LifecycleStateTest, TransitionInvokesCorrectHook) {
    TestLifecycleActor a;
    EXPECT_TRUE(a.transition(LifecycleState::kActive));
    EXPECT_EQ(a.start_calls, 1);
    EXPECT_EQ(a.drain_calls, 0);
    EXPECT_TRUE(a.transition(LifecycleState::kDraining));
    EXPECT_EQ(a.drain_calls, 1);
    EXPECT_TRUE(a.transition(LifecycleState::kStopping));
    EXPECT_EQ(a.stop_calls, 1);
    EXPECT_TRUE(a.transition(LifecycleState::kStopped));
    EXPECT_EQ(a.deactivate_calls, 1);
    EXPECT_EQ(a.fail_calls, 0);
    EXPECT_EQ(a.recover_calls, 0);
    EXPECT_EQ(a.restart_calls, 0);
}

TEST(LifecycleStateTest, IncarnationBumps) {
    TestLifecycleActor a;
    EXPECT_EQ(a.incarnation(), 0u);
    a.bump_incarnation();
    EXPECT_EQ(a.incarnation(), 1u);
    a.bump_incarnation();
    EXPECT_EQ(a.incarnation(), 2u);
}

// -- kQuarantined transition tests --

TEST(LifecycleStateTest, ActiveToQuarantined) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    bool ok = a.transition(LifecycleState::kQuarantined);
    EXPECT_TRUE(ok);
    EXPECT_EQ(a.state(), LifecycleState::kQuarantined);
}

TEST(LifecycleStateTest, FailedToQuarantined) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    a.transition(LifecycleState::kFailed);
    bool ok = a.transition(LifecycleState::kQuarantined);
    EXPECT_TRUE(ok);
    EXPECT_EQ(a.state(), LifecycleState::kQuarantined);
}

TEST(LifecycleStateTest, RecoveringToQuarantined) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    a.transition(LifecycleState::kFailed);
    a.transition(LifecycleState::kRecovering);
    bool ok = a.transition(LifecycleState::kQuarantined);
    EXPECT_TRUE(ok);
    EXPECT_EQ(a.state(), LifecycleState::kQuarantined);
}

TEST(LifecycleStateTest, QuarantinedToStopped) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    a.transition(LifecycleState::kQuarantined);
    bool ok = a.transition(LifecycleState::kStopped);
    EXPECT_TRUE(ok);
    EXPECT_EQ(a.state(), LifecycleState::kStopped);
}

TEST(LifecycleStateTest, QuarantinedToActiveIllegal) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    a.transition(LifecycleState::kQuarantined);
    bool ok = a.transition(LifecycleState::kActive);
    EXPECT_FALSE(ok);
    EXPECT_EQ(a.state(), LifecycleState::kQuarantined);
}

TEST(LifecycleStateTest, QuarantinedNoUserMsgs) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    a.transition(LifecycleState::kQuarantined);
    EXPECT_FALSE(a.accepts_user_msgs());
}

TEST(LifecycleStateTest, QuarantinedAcceptsSystemMsgs) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    a.transition(LifecycleState::kQuarantined);
    EXPECT_TRUE(a.accepts_system_msgs());
}

TEST(LifecycleStateTest, QuarantinedStateString) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    a.transition(LifecycleState::kQuarantined);
    EXPECT_EQ(std::string(a.state_string()), "quarantined");
}

TEST(LifecycleStateTest, StartingToQuarantinedIllegal) {
    TestLifecycleActor a;
    // kStarting cannot transition directly to kQuarantined.
    bool ok = a.transition(LifecycleState::kQuarantined);
    EXPECT_FALSE(ok);
    EXPECT_EQ(a.state(), LifecycleState::kStarting);
}