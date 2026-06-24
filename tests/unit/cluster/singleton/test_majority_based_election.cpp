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
#include <hpactor/cluster/singleton/majority_based_election.hpp>
#include <hpactor/cluster/singleton/singleton_election.hpp>

#include <string>
#include <vector>

namespace hpactor::cluster::singleton {

static std::vector<std::string> nodes(std::initializer_list<std::string> n) {
    return std::vector<std::string>{n};
}

TEST(MajorityBasedElectionTest, SoloNodeHasMajority) {
    MajorityBasedElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto alive = nodes({"node-a"});
    // Solo node votes for itself
    election.record_vote("test-singleton", "node-a", "node-a");
    auto result = election.elect(id, alive);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "node-a"); // 1/1 = majority
}

TEST(MajorityBasedElectionTest, TwoOfThreeIsMajority) {
    MajorityBasedElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto alive = nodes({"node-a", "node-b", "node-c"});
    // 2 out of 3 vote for node-b
    election.record_vote("test-singleton", "node-a", "node-b");
    election.record_vote("test-singleton", "node-b", "node-b");
    auto result = election.elect(id, alive);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "node-b"); // 2/3 > 1/2 majority
}

TEST(MajorityBasedElectionTest, ThreeOfFiveIsMajority) {
    MajorityBasedElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto alive = nodes({"n1", "n2", "n3", "n4", "n5"});
    election.record_vote("test-singleton", "n1", "n1");
    election.record_vote("test-singleton", "n2", "n1");
    election.record_vote("test-singleton", "n3", "n1");
    // 3/5 > 2.5 majority
    auto result = election.elect(id, alive);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "n1");
}

TEST(MajorityBasedElectionTest, NoMajorityReturnsNullopt) {
    MajorityBasedElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto alive = nodes({"node-a", "node-b", "node-c"});
    // 1 vote each — no majority
    election.record_vote("test-singleton", "node-a", "node-a");
    election.record_vote("test-singleton", "node-b", "node-b");
    election.record_vote("test-singleton", "node-c", "node-c");
    auto result = election.elect(id, alive);
    EXPECT_FALSE(result.has_value());
}

TEST(MajorityBasedElectionTest, TieWithTwoNodesNoMajority) {
    MajorityBasedElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto alive = nodes({"node-a", "node-b"});
    election.record_vote("test-singleton", "node-a", "node-a");
    election.record_vote("test-singleton", "node-b", "node-b");
    auto result = election.elect(id, alive);
    // 1/2 is not > 1/2 — no majority in even split
    EXPECT_FALSE(result.has_value());
}

TEST(MajorityBasedElectionTest, EmptyAliveNodesReturnsNullopt) {
    MajorityBasedElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto result = election.elect(id, {});
    EXPECT_FALSE(result.has_value());
}

TEST(MajorityBasedElectionTest, NoVotesRecordedReturnsNullopt) {
    MajorityBasedElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto alive = nodes({"node-a", "node-b", "node-c"});
    auto result = election.elect(id, alive);
    EXPECT_FALSE(result.has_value());
}

TEST(MajorityBasedElectionTest, NodeRemovalFlipsMajority) {
    MajorityBasedElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto alive3 = nodes({"node-a", "node-b", "node-c"});
    // node-a had 2/3 votes
    election.record_vote("test-singleton", "node-a", "node-a");
    election.record_vote("test-singleton", "node-b", "node-a");
    election.record_vote("test-singleton", "node-c", "node-c");
    auto result3 = election.elect(id, alive3);
    ASSERT_TRUE(result3.has_value());
    EXPECT_EQ(*result3, "node-a");

    // node-a goes down — remaining nodes are b and c. node-c now has majority
    // (1/1 active votes, only 2 alive) But vote from node-a is now stale since
    // node-a is gone
    election.on_peer_down("node-a");
    auto alive2 = nodes({"node-b", "node-c"});
    auto result2 = election.elect(id, alive2);
    // node-c had 1 vote; node-a's vote for node-a is stale. Need ≥ majority of
    // 2 = 2 node-b has 0 active votes, node-c has 1 (its own) → no majority
    EXPECT_FALSE(result2.has_value())
        << "stale votes from dead node should not count";
}

TEST(MajorityBasedElectionTest, OnPeerDownClearsVotesFromDeadNode) {
    MajorityBasedElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto alive = nodes({"node-x", "node-y", "node-z"});
    // All 3 vote for node-x
    election.record_vote("test-singleton", "node-x", "node-x");
    election.record_vote("test-singleton", "node-y", "node-x");
    election.record_vote("test-singleton", "node-z", "node-x");
    auto before = election.elect(id, alive);
    ASSERT_TRUE(before.has_value());
    EXPECT_EQ(*before, "node-x");

    // node-x goes down — its own vote is removed
    election.on_peer_down("node-x");
    auto after_alive = nodes({"node-y", "node-z"});
    auto after = election.elect(id, after_alive);
    // 2 votes for node-x, but node-x is dead. Need to re-vote.
    // Only y and z are alive, and neither has voted for each other.
    EXPECT_FALSE(after.has_value()) << "dead node cannot be elected even with votes";
}

TEST(MajorityBasedElectionTest, RecordVoteOverwritesPreviousVote) {
    MajorityBasedElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto alive = nodes({"node-a", "node-b", "node-c"});
    // node-a initially votes for node-c, then changes to node-b
    election.record_vote("test-singleton", "node-a", "node-c");
    election.record_vote("test-singleton", "node-b", "node-b");
    election.record_vote("test-singleton", "node-a", "node-b"); // overwrite
    // Now: b votes for b, a votes for b → 2/3 majority for b
    auto result = election.elect(id, alive);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "node-b");
}

TEST(MajorityBasedElectionTest, DifferentSingletonNamesAreIndependent) {
    MajorityBasedElection election;
    auto alive = nodes({"node-a", "node-b", "node-c"});

    SingletonIdentity id1{"singleton-1", 0};
    election.record_vote("singleton-1", "node-a", "node-a");
    election.record_vote("singleton-1", "node-b", "node-a");

    SingletonIdentity id2{"singleton-2", 0};
    election.record_vote("singleton-2", "node-a", "node-b");
    election.record_vote("singleton-2", "node-b", "node-b");

    auto r1 = election.elect(id1, alive);
    auto r2 = election.elect(id2, alive);
    ASSERT_TRUE(r1.has_value());
    ASSERT_TRUE(r2.has_value());
    EXPECT_EQ(*r1, "node-a");
    EXPECT_EQ(*r2, "node-b");
}

} // namespace hpactor::cluster::singleton
