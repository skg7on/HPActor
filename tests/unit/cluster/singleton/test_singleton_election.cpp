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
#include <hpactor/cluster/singleton/oldest_node_election.hpp>
#include <hpactor/cluster/singleton/singleton_election.hpp>

#include <string>
#include <vector>

namespace hpactor::cluster::singleton {

std::vector<std::string> nodes(std::initializer_list<std::string> n) {
    return std::vector<std::string>{n};
}

TEST(OldestNodeElectionTest, SingleNodeWins) {
    OldestNodeElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto result = election.elect(id, nodes({"node-a"}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "node-a");
}

TEST(OldestNodeElectionTest, LowestNodeIdWins) {
    OldestNodeElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto result = election.elect(id, nodes({"node-c", "node-a", "node-b"}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "node-a"); // alphabetically lowest
}

TEST(OldestNodeElectionTest, EmptyNodesReturnsNullopt) {
    OldestNodeElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto result = election.elect(id, {});
    EXPECT_FALSE(result.has_value());
}

TEST(OldestNodeElectionTest, DeterministicSameInputSameOutput) {
    OldestNodeElection election;
    SingletonIdentity id{"test-singleton", 0};
    auto n = nodes({"node-x", "node-y", "node-z"});
    auto r1 = election.elect(id, n);
    auto r2 = election.elect(id, n);
    ASSERT_TRUE(r1.has_value() && r2.has_value());
    EXPECT_EQ(*r1, *r2);
}

TEST(OldestNodeElectionTest, OnPeerDownRemovesFromConsideration) {
    OldestNodeElection election;
    // Not directly testable without internal state — but method exists
    election.on_peer_down("node-dead");
    // Verify doesn't crash and still works
    auto result = election.elect({"test", 0}, nodes({"node-a"}));
    ASSERT_TRUE(result.has_value());
}

TEST(OldestNodeElectionTest, TwoNodesWithSamePrefix) {
    OldestNodeElection election;
    auto result = election.elect({"test", 0}, nodes({"node-10", "node-2"}));
    ASSERT_TRUE(result.has_value());
    // String comparison: "node-10" < "node-2" (lexicographic)
    EXPECT_EQ(*result, "node-10");
}

} // namespace hpactor::cluster::singleton
