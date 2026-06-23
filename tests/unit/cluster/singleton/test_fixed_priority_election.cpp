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
#include <hpactor/cluster/singleton/fixed_priority_election.hpp>
#include <hpactor/cluster/singleton/singleton_election.hpp>

#include <string>
#include <vector>

namespace hpactor::cluster::singleton {

static std::vector<std::string> nodes(std::initializer_list<std::string> n) {
    return std::vector<std::string>{n};
}

TEST(FixedPriorityElectionTest, HighestPriorityWins) {
    std::unordered_map<std::string, int> prio = {
        {"node-a", 10}, {"node-b", 20}, {"node-c", 5}};
    FixedPriorityElection election(prio);
    SingletonIdentity id{"test-singleton", 0};
    auto result = election.elect(id, nodes({"node-a", "node-b", "node-c"}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "node-b"); // priority 20
}

TEST(FixedPriorityElectionTest, OnlyAliveNodesConsidered) {
    std::unordered_map<std::string, int> prio = {
        {"node-high", 100}, {"node-mid", 50}, {"node-low", 1}};
    FixedPriorityElection election(prio);
    SingletonIdentity id{"test-singleton", 0};
    // node-high is not in alive list
    auto result = election.elect(id, nodes({"node-mid", "node-low"}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "node-mid"); // priority 50 is highest among alive
}

TEST(FixedPriorityElectionTest, TieBrokenByLowestNodeId) {
    std::unordered_map<std::string, int> prio = {
        {"node-x", 10}, {"node-y", 10}, {"node-z", 10}};
    FixedPriorityElection election(prio);
    SingletonIdentity id{"test-singleton", 0};
    auto result = election.elect(id, nodes({"node-z", "node-x", "node-y"}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "node-x"); // lexicographically lowest
}

TEST(FixedPriorityElectionTest, EmptyAliveNodesReturnsNullopt) {
    std::unordered_map<std::string, int> prio = {{"node-a", 10}};
    FixedPriorityElection election(prio);
    SingletonIdentity id{"test-singleton", 0};
    auto result = election.elect(id, {});
    EXPECT_FALSE(result.has_value());
}

TEST(FixedPriorityElectionTest, NodesNotInPriorityMapAreExcluded) {
    std::unordered_map<std::string, int> prio = {{"node-a", 10}};
    FixedPriorityElection election(prio);
    SingletonIdentity id{"test-singleton", 0};
    // node-b is alive but has no priority entry — excluded
    auto result = election.elect(id, nodes({"node-b"}));
    EXPECT_FALSE(result.has_value());
}

TEST(FixedPriorityElectionTest, OnPeerDownRemovesFromKnownDead) {
    std::unordered_map<std::string, int> prio = {{"node-a", 10}, {"node-b", 20}};
    FixedPriorityElection election(prio);
    election.on_peer_down("node-b");
    // After node-b is marked dead, but elect only considers alive_nodes
    SingletonIdentity id{"test-singleton", 0};
    auto result = election.elect(id, nodes({"node-a", "node-b"}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "node-b"); // node-b is in alive_nodes, still eligible
}

TEST(FixedPriorityElectionTest, DeterministicSameInputSameOutput) {
    std::unordered_map<std::string, int> prio = {
        {"node-p", 30}, {"node-q", 20}, {"node-r", 10}};
    FixedPriorityElection election(prio);
    SingletonIdentity id{"test-singleton", 0};
    auto n = nodes({"node-p", "node-q", "node-r"});
    auto r1 = election.elect(id, n);
    auto r2 = election.elect(id, n);
    ASSERT_TRUE(r1.has_value() && r2.has_value());
    EXPECT_EQ(*r1, *r2);
}

TEST(FixedPriorityElectionTest, SingleNodeWithPriorityWins) {
    std::unordered_map<std::string, int> prio = {{"lonely", 42}};
    FixedPriorityElection election(prio);
    SingletonIdentity id{"test-singleton", 0};
    auto result = election.elect(id, nodes({"lonely"}));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "lonely");
}

} // namespace hpactor::cluster::singleton
