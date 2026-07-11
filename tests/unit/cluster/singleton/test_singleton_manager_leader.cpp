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
#include <hpactor/cluster/singleton/fake_leadership_backend.hpp>
#include <hpactor/cluster/singleton/leadership_backend_adapter.hpp>
#include <hpactor/cluster/singleton/oldest_node_election.hpp>
#include <hpactor/cluster/singleton/singleton_manager.hpp>

#include <memory>
#include <string>
#include <vector>

namespace hpactor::cluster::singleton {

using namespace std::chrono_literals;

class SingletonManagerLeaderTest : public ::testing::Test {
  protected:
    void SetUp() override {
        backend_ = std::make_unique<FakeLeadershipBackend>();
    }

    /// rief Create a SingletonManagerCore backed by the fake backend.
    SingletonManagerCore make_manager(const std::string& node_id) {
        auto adapter =
            std::make_unique<LeadershipBackendAdapter>(node_id, backend_.get());
        return SingletonManagerCore(node_id, std::move(adapter));
    }

    std::unique_ptr<FakeLeadershipBackend> backend_;
};

TEST_F(SingletonManagerLeaderTest, BackendGrantActivatesSingleton) {
    backend_->force_grant("shard-coordinator", "node-self", 10s);

    // Re-create with the backend that has a pre-granted lease
    auto mgr = make_manager("node-self");
    mgr.register_singleton(SingletonIdentity{"shard-coordinator", 0});
    mgr.on_node_state_change({"node-self"});

    EXPECT_EQ(mgr.get_state("shard-coordinator"), SingletonState::Active);
}

TEST_F(SingletonManagerLeaderTest, FencingTokenFromBackend) {
    backend_->force_grant("shard-coordinator", "node-self", 10s);

    auto mgr = make_manager("node-self");
    mgr.register_singleton(SingletonIdentity{"shard-coordinator", 0});
    mgr.on_node_state_change({"node-self"});

    auto token = mgr.get_fencing_token("shard-coordinator");
    EXPECT_GT(token, 0u);
}

TEST_F(SingletonManagerLeaderTest, OtherOwnerKeepsSelfInStandby) {
    backend_->force_grant("shard-coordinator", "node-other", 10s);

    auto mgr = make_manager("node-self");
    mgr.register_singleton(SingletonIdentity{"shard-coordinator", 0});
    mgr.on_node_state_change({"node-self", "node-other"});

    EXPECT_EQ(mgr.get_state("shard-coordinator"), SingletonState::Standby);
}

TEST_F(SingletonManagerLeaderTest, BackendUnavailableKeepsStandby) {
    backend_->simulate_unavailable(true);

    auto mgr = make_manager("node-self");
    mgr.register_singleton(SingletonIdentity{"shard-coordinator", 0});
    mgr.on_node_state_change({"node-self"});

    EXPECT_EQ(mgr.get_state("shard-coordinator"), SingletonState::Standby);
}

TEST_F(SingletonManagerLeaderTest, ExistingLocalElectionTestsStillPass) {
    // Verify that OldestNodeElection still works unchanged
    auto election = std::make_unique<OldestNodeElection>();
    SingletonManagerCore mgr("node-self", std::move(election));
    mgr.register_singleton(SingletonIdentity{"test-singleton", 0});
    mgr.on_node_state_change({"node-self"});

    EXPECT_EQ(mgr.get_state("test-singleton"), SingletonState::Active);
    // Local election increments token locally (get_fencing_token returns 0)
    auto token = mgr.get_fencing_token("test-singleton");
    EXPECT_GT(token, 0u); // locally incremented to 1
}

} // namespace hpactor::cluster::singleton
