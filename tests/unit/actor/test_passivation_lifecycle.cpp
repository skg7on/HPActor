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
#include <hpactor/actor/lifecycle_state.hpp>

using namespace hpactor;

TEST(PassivationLifecycleTest, ActiveCanTransitionToPassivating) {
    const auto& def = kStateMachine[static_cast<int>(LifecycleState::kActive)];
    bool found = false;
    for (uint8_t i = 0; i < def.num_transitions; ++i) {
        if (def.transitions[i] == LifecycleState::kPassivating) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "kActive must be able to transition to kPassivating";
}

TEST(PassivationLifecycleTest, PassivatingAcceptsNoUserMessages) {
    const auto& def =
        kStateMachine[static_cast<int>(LifecycleState::kPassivating)];
    EXPECT_FALSE(def.accepts_user_msgs);
    EXPECT_TRUE(def.accepts_system_msgs);
    EXPECT_STREQ(def.name, "passivating");
}

TEST(PassivationLifecycleTest, PassivatingTransitions) {
    const auto& def =
        kStateMachine[static_cast<int>(LifecycleState::kPassivating)];
    EXPECT_EQ(def.num_transitions, 2);
    bool has_passivated = false;
    bool has_failed = false;
    for (uint8_t i = 0; i < def.num_transitions; ++i) {
        if (def.transitions[i] == LifecycleState::kPassivated) {
            has_passivated = true;
        }
        if (def.transitions[i] == LifecycleState::kFailed) {
            has_failed = true;
        }
    }
    EXPECT_TRUE(has_passivated);
    EXPECT_TRUE(has_failed);
}

TEST(PassivationLifecycleTest, PassivatedTransitions) {
    const auto& def = kStateMachine[static_cast<int>(LifecycleState::kPassivated)];
    EXPECT_EQ(def.num_transitions, 3);
    bool has_recovering = false;
    bool has_stopped = false;
    bool has_failed = false;
    for (uint8_t i = 0; i < def.num_transitions; ++i) {
        if (def.transitions[i] == LifecycleState::kRecovering) {
            has_recovering = true;
        }
        if (def.transitions[i] == LifecycleState::kStopped) {
            has_stopped = true;
        }
        if (def.transitions[i] == LifecycleState::kFailed) {
            has_failed = true;
        }
    }
    EXPECT_TRUE(has_recovering);
    EXPECT_TRUE(has_stopped);
    EXPECT_TRUE(has_failed);
}

TEST(PassivationLifecycleTest, PassivatedAcceptsNoUserMessages) {
    const auto& def = kStateMachine[static_cast<int>(LifecycleState::kPassivated)];
    EXPECT_FALSE(def.accepts_user_msgs);
    EXPECT_TRUE(def.accepts_system_msgs);
    EXPECT_STREQ(def.name, "passivated");
}

TEST(PassivationLifecycleTest, StateMachineHasExactlyTenEntries) {
    EXPECT_EQ(sizeof(kStateMachine) / sizeof(StateDef), 10);
}

TEST(PassivationLifecycleTest, IllegalDirectTransitionToActive) {
    const auto& def = kStateMachine[static_cast<int>(LifecycleState::kPassivated)];
    bool has_active = false;
    for (uint8_t i = 0; i < def.num_transitions; ++i) {
        if (def.transitions[i] == LifecycleState::kActive) {
            has_active = true;
        }
    }
    EXPECT_FALSE(has_active) << "kPassivated must NOT transition directly to "
                                "kActive";
}

TEST(PassivationLifecycleTest, EnumValuesAreCorrect) {
    EXPECT_EQ(static_cast<uint8_t>(LifecycleState::kPassivating), 8);
    EXPECT_EQ(static_cast<uint8_t>(LifecycleState::kPassivated), 9);
}
