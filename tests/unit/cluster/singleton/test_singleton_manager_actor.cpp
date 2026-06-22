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
#include <hpactor/cluster/singleton/singleton_manager_actor.hpp>

namespace hpactor::cluster::singleton {

TEST(SingletonManagerActorTest, ConstructionSetsSelfNode) {
    auto election = std::make_unique<OldestNodeElection>();
    SingletonManagerActor actor("node-1", std::move(election));
    EXPECT_EQ(actor.self_node(), "node-1");
}

TEST(SingletonManagerActorTest, RegisterSingleton) {
    auto election = std::make_unique<OldestNodeElection>();
    SingletonManagerActor actor("node-1", std::move(election));
    actor.register_singleton(SingletonIdentity{"shard-coordinator", 0});
    auto registered = actor.core().get_registered();
    EXPECT_EQ(registered.size(), 1u);
    EXPECT_EQ(registered[0].name, "shard-coordinator");
}

TEST(SingletonManagerActorTest, OnNodeStateChangeWithSelfAloneActivates) {
    auto election = std::make_unique<OldestNodeElection>();
    SingletonManagerActor actor("node-1", std::move(election));
    actor.register_singleton(SingletonIdentity{"test-singleton", 0});
    std::vector<std::string> alive = {"node-1"};
    actor.on_node_state_change(alive);
    EXPECT_EQ(actor.core().get_state("test-singleton"), SingletonState::Active);
}

TEST(SingletonManagerActorTest, OnNodeStateChangeWithOthersStaysStandby) {
    auto election = std::make_unique<OldestNodeElection>();
    SingletonManagerActor actor("node-2", std::move(election));
    actor.register_singleton(SingletonIdentity{"test-singleton", 0});
    std::vector<std::string> alive = {"node-1", "node-2"};
    actor.on_node_state_change(alive);
    EXPECT_EQ(actor.core().get_state("test-singleton"), SingletonState::Standby);
}

TEST(SingletonManagerActorTest, BeginAndCompleteDrain) {
    auto election = std::make_unique<OldestNodeElection>();
    SingletonManagerActor actor("node-1", std::move(election));
    actor.register_singleton(SingletonIdentity{"test-singleton", 0});
    std::vector<std::string> alive = {"node-1"};
    actor.on_node_state_change(alive);
    EXPECT_EQ(actor.core().get_state("test-singleton"), SingletonState::Active);
    EXPECT_TRUE(actor.begin_drain("test-singleton"));
    EXPECT_EQ(actor.core().get_state("test-singleton"), SingletonState::Draining);
    EXPECT_TRUE(actor.complete_drain("test-singleton"));
    EXPECT_EQ(actor.core().get_state("test-singleton"), SingletonState::Standby);
}

} // namespace hpactor::cluster::singleton
