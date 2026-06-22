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
#include <hpactor/cluster/singleton/singleton_manager.hpp>

#include <memory>
#include <string>
#include <vector>

namespace hpactor::cluster::singleton {

class SingletonManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto election = std::make_unique<OldestNodeElection>();
        manager_ = std::make_unique<SingletonManagerCore>("node-self",
                                                          std::move(election));
    }
    std::unique_ptr<SingletonManagerCore> manager_;
};

TEST_F(SingletonManagerTest, SelfNodeIsStored) {
    EXPECT_EQ(manager_->self_node(), "node-self");
}

TEST_F(SingletonManagerTest, InitiallyNoSingletons) {
    EXPECT_EQ(manager_->singleton_count(), 0);
}

TEST_F(SingletonManagerTest, RegisterSingletonStartsInStandby) {
    SingletonIdentity id{"test-singleton", 0};
    manager_->register_singleton(id);
    EXPECT_EQ(manager_->singleton_count(), 1);
    EXPECT_EQ(manager_->get_state("test-singleton"), SingletonState::Standby);
}

TEST_F(SingletonManagerTest, ElectionPromotesSelfToActive) {
    SingletonIdentity id{"test-singleton", 0};
    manager_->register_singleton(id);

    std::vector<std::string> alive_nodes = {"node-self"};
    manager_->on_node_state_change(alive_nodes);

    // Self is the only node — should win election and become Active
    EXPECT_EQ(manager_->get_state("test-singleton"), SingletonState::Active);
}

TEST_F(SingletonManagerTest, ElectionMakesSelfStandbyWhenNotWinner) {
    SingletonIdentity id{"test-singleton", 0};
    manager_->register_singleton(id);

    // "node-aa" is alphabetically before "node-self"
    std::vector<std::string> alive_nodes = {"node-aa", "node-self"};
    manager_->on_node_state_change(alive_nodes);

    // Self didn't win — should be Standby
    EXPECT_EQ(manager_->get_state("test-singleton"), SingletonState::Standby);
}

TEST_F(SingletonManagerTest, ActivatingTransitionsToActive) {
    SingletonIdentity id{"test-singleton", 0};
    manager_->register_singleton(id);

    std::vector<std::string> alive_nodes = {"node-self"};
    manager_->on_node_state_change(alive_nodes);

    EXPECT_EQ(manager_->get_state("test-singleton"), SingletonState::Active);
}

TEST_F(SingletonManagerTest, FencingTokenIncrementsOnActivation) {
    SingletonIdentity id{"test-singleton", 0};
    manager_->register_singleton(id);

    std::vector<std::string> alive = {"node-self"};
    manager_->on_node_state_change(alive);

    auto token = manager_->get_fencing_token("test-singleton");
    EXPECT_GT(token, 0);
}

TEST_F(SingletonManagerTest, TransitionActiveToDrainingToStandby) {
    SingletonIdentity id{"test-singleton", 0};
    manager_->register_singleton(id);

    // Become active
    manager_->on_node_state_change({"node-self"});
    EXPECT_EQ(manager_->get_state("test-singleton"), SingletonState::Active);

    // Begin drain
    EXPECT_TRUE(manager_->begin_drain("test-singleton"));
    EXPECT_EQ(manager_->get_state("test-singleton"), SingletonState::Draining);

    // Complete drain
    EXPECT_TRUE(manager_->complete_drain("test-singleton"));
    EXPECT_EQ(manager_->get_state("test-singleton"), SingletonState::Standby);
}

TEST_F(SingletonManagerTest, UnknownSingletonReturnsStandby) {
    EXPECT_EQ(manager_->get_state("nonexistent"), SingletonState::Standby);
}

TEST_F(SingletonManagerTest, MultipleSingletonsTrackedIndependently) {
    SingletonIdentity a{"singleton-a", 0};
    SingletonIdentity b{"singleton-b", 0};
    manager_->register_singleton(a);
    manager_->register_singleton(b);

    // Only self — both win election
    manager_->on_node_state_change({"node-self"});

    EXPECT_EQ(manager_->get_state("singleton-a"), SingletonState::Active);
    EXPECT_EQ(manager_->get_state("singleton-b"), SingletonState::Active);
    EXPECT_EQ(manager_->singleton_count(), 2);
}

} // namespace hpactor::cluster::singleton
