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
#include <hpactor/cluster/cluster_node_state.hpp>

namespace hpactor::cluster {

TEST(ClusterNodeStateTest, AllStatesAreDefined) {
    EXPECT_NE(static_cast<uint8_t>(ClusterNodeState::Joining),
              static_cast<uint8_t>(ClusterNodeState::Alive));
    EXPECT_NE(static_cast<uint8_t>(ClusterNodeState::Alive),
              static_cast<uint8_t>(ClusterNodeState::Suspect));
}

TEST(ClusterNodeStateTest, AliveAcceptsPlacement) {
    EXPECT_TRUE(is_alive(ClusterNodeState::Alive));
    EXPECT_FALSE(is_alive(ClusterNodeState::Suspect));
    EXPECT_FALSE(is_alive(ClusterNodeState::Unreachable));
    EXPECT_FALSE(is_alive(ClusterNodeState::Down));
    EXPECT_FALSE(is_alive(ClusterNodeState::Quarantined));
    EXPECT_FALSE(is_alive(ClusterNodeState::Joining));
    EXPECT_FALSE(is_alive(ClusterNodeState::Leaving));
    EXPECT_FALSE(is_alive(ClusterNodeState::Removed));
}

TEST(ClusterNodeStateTest, DownAndRemovedAreTerminal) {
    EXPECT_TRUE(is_terminal(ClusterNodeState::Down));
    EXPECT_TRUE(is_terminal(ClusterNodeState::Removed));
    EXPECT_FALSE(is_terminal(ClusterNodeState::Alive));
    EXPECT_FALSE(is_terminal(ClusterNodeState::Suspect));
    EXPECT_FALSE(is_terminal(ClusterNodeState::Quarantined));
}

TEST(ClusterNodeStateTest, QuarantinedNoAutoRecovery) {
    EXPECT_FALSE(is_alive(ClusterNodeState::Quarantined));
    EXPECT_FALSE(is_terminal(ClusterNodeState::Quarantined));
}

// Transitions — Joining
TEST(ClusterNodeStateTest, TransitionJoiningToAlive) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Joining, ClusterNodeState::Alive));
}

// Transitions — Alive
TEST(ClusterNodeStateTest, TransitionAliveToSuspect) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Alive, ClusterNodeState::Suspect));
}
TEST(ClusterNodeStateTest, TransitionAliveToUnreachable) {
    EXPECT_TRUE(
        can_transition(ClusterNodeState::Alive, ClusterNodeState::Unreachable));
}
TEST(ClusterNodeStateTest, TransitionAliveToQuarantined) {
    EXPECT_TRUE(
        can_transition(ClusterNodeState::Alive, ClusterNodeState::Quarantined));
}
TEST(ClusterNodeStateTest, TransitionAliveToLeaving) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Alive, ClusterNodeState::Leaving));
}

// Transitions — Suspect
TEST(ClusterNodeStateTest, TransitionSuspectBackToAlive) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Suspect, ClusterNodeState::Alive));
}
TEST(ClusterNodeStateTest, TransitionSuspectToUnreachable) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Suspect,
                               ClusterNodeState::Unreachable));
}
TEST(ClusterNodeStateTest, TransitionSuspectToQuarantined) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Suspect,
                               ClusterNodeState::Quarantined));
}

// Transitions — Unreachable
TEST(ClusterNodeStateTest, TransitionUnreachableToAlive) {
    EXPECT_TRUE(
        can_transition(ClusterNodeState::Unreachable, ClusterNodeState::Alive));
}
TEST(ClusterNodeStateTest, TransitionUnreachableToDown) {
    EXPECT_TRUE(
        can_transition(ClusterNodeState::Unreachable, ClusterNodeState::Down));
}

// Transitions — Leaving → Down
TEST(ClusterNodeStateTest, TransitionLeavingToDown) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Leaving, ClusterNodeState::Down));
}

// Transitions — Down → Removed
TEST(ClusterNodeStateTest, TransitionDownToRemoved) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Down, ClusterNodeState::Removed));
}

// Transitions — Quarantined → Joining (operator clear)
TEST(ClusterNodeStateTest, QuarantinedToJoiningViaOperator) {
    EXPECT_TRUE(can_transition(ClusterNodeState::Quarantined,
                               ClusterNodeState::Joining));
}

// Illegal transitions
TEST(ClusterNodeStateTest, NoTransitionFromDownToAlive) {
    EXPECT_FALSE(can_transition(ClusterNodeState::Down, ClusterNodeState::Alive));
}
TEST(ClusterNodeStateTest, NoTransitionFromRemoved) {
    EXPECT_FALSE(
        can_transition(ClusterNodeState::Removed, ClusterNodeState::Joining));
    EXPECT_FALSE(can_transition(ClusterNodeState::Removed, ClusterNodeState::Alive));
}
TEST(ClusterNodeStateTest, NoTransitionFromQuarantinedToAlive) {
    EXPECT_FALSE(
        can_transition(ClusterNodeState::Quarantined, ClusterNodeState::Alive));
}
TEST(ClusterNodeStateTest, NoSelfTransition) {
    EXPECT_FALSE(can_transition(ClusterNodeState::Alive, ClusterNodeState::Alive));
    EXPECT_FALSE(can_transition(ClusterNodeState::Down, ClusterNodeState::Down));
}

// to_string
TEST(ClusterNodeStateTest, ToStringReturnsExpected) {
    EXPECT_STREQ(to_string(ClusterNodeState::Alive), "alive");
    EXPECT_STREQ(to_string(ClusterNodeState::Down), "down");
    EXPECT_STREQ(to_string(ClusterNodeState::Quarantined), "quarantined");
    EXPECT_STREQ(to_string(ClusterNodeState::Joining), "joining");
    EXPECT_STREQ(to_string(ClusterNodeState::Suspect), "suspect");
    EXPECT_STREQ(to_string(ClusterNodeState::Unreachable), "unreachable");
    EXPECT_STREQ(to_string(ClusterNodeState::Leaving), "leaving");
    EXPECT_STREQ(to_string(ClusterNodeState::Removed), "removed");
}

} // namespace hpactor::cluster
