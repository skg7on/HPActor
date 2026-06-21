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
#include <hpactor/cluster/cluster_failure_model.hpp>
#include <hpactor/cluster/route_invalidation.hpp>

namespace hpactor::cluster {

class ClusterFailureModelTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // model_ is already default-constructed by the fixture.
    }
    ClusterFailureModel model_;
};

TEST_F(ClusterFailureModelTest, NewModelHasEmptyNodeMap) {
    EXPECT_EQ(model_.node_count(), 0);
}

TEST_F(ClusterFailureModelTest, RegisterNodeAddsToMap) {
    ClusterNodeIdentity id;
    id.node_id = "node-1";
    id.incarnation = 1;
    id.process_start_id = 100;
    id.cluster_id = "test-cluster";

    EXPECT_TRUE(model_.register_node(id));
    EXPECT_EQ(model_.node_count(), 1);
    EXPECT_EQ(model_.get_state("node-1"), ClusterNodeState::Joining);
}

TEST_F(ClusterFailureModelTest, TransitionNodeChangesState) {
    ClusterNodeIdentity id;
    id.node_id = "node-1";
    id.cluster_id = "test-cluster";

    model_.register_node(id);
    auto result = model_.transition("node-1", ClusterNodeState::Alive,
                                    "handshake complete");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(model_.get_state("node-1"), ClusterNodeState::Alive);
}

TEST_F(ClusterFailureModelTest, TransitionRejectsIllegalMove) {
    ClusterNodeIdentity id;
    id.node_id = "node-1";
    id.cluster_id = "test-cluster";

    model_.register_node(id);
    model_.transition("node-1", ClusterNodeState::Alive, "handshake complete");

    // Alive -> Removed is illegal (must go through Down first)
    auto result =
        model_.transition("node-1", ClusterNodeState::Removed, "bad transition");
    EXPECT_FALSE(result.success);
    EXPECT_EQ(model_.get_state("node-1"), ClusterNodeState::Alive);
}

TEST_F(ClusterFailureModelTest, TransitionToDownTriggersInvalidation) {
    ClusterNodeIdentity id;
    id.node_id = "node-1";
    id.cluster_id = "test-cluster";

    model_.register_node(id);
    model_.transition("node-1", ClusterNodeState::Alive, "ready");
    model_.transition("node-1", ClusterNodeState::Unreachable, "partition");
    auto result = model_.transition("node-1", ClusterNodeState::Down,
                                    "prolonged unreachable");

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.routes_invalidated);
    EXPECT_EQ(model_.get_state("node-1"), ClusterNodeState::Down);
}

TEST_F(ClusterFailureModelTest, RegisterDuplicateIdentityQuarantines) {
    ClusterNodeIdentity id;
    id.node_id = "node-1";
    id.incarnation = 1;
    id.process_start_id = 100;
    id.cluster_id = "test-cluster";

    model_.register_node(id);
    model_.transition("node-1", ClusterNodeState::Alive, "ready");

    ClusterNodeIdentity duplicate;
    duplicate.node_id = "node-1";
    duplicate.incarnation = 1;
    duplicate.process_start_id = 200; // different process
    duplicate.cluster_id = "test-cluster";

    auto status = model_.check_identity_conflict(duplicate);
    EXPECT_TRUE(status.conflict_detected);
    EXPECT_EQ(status.resolution, IdentityConflictResolution::QuarantineBoth);
}

TEST_F(ClusterFailureModelTest, HigherIncarnationFencesOld) {
    ClusterNodeIdentity id_v1;
    id_v1.node_id = "node-1";
    id_v1.incarnation = 1;
    id_v1.process_start_id = 100;
    id_v1.cluster_id = "test-cluster";

    model_.register_node(id_v1);
    model_.transition("node-1", ClusterNodeState::Alive, "ready");

    ClusterNodeIdentity id_v2;
    id_v2.node_id = "node-1";
    id_v2.incarnation = 2; // higher
    id_v2.process_start_id = 200;
    id_v2.cluster_id = "test-cluster";

    auto status = model_.check_identity_conflict(id_v2);
    EXPECT_FALSE(status.conflict_detected);
    EXPECT_TRUE(status.fence_old);
}

TEST_F(ClusterFailureModelTest, UnknownNodeReturnsSentinel) {
    EXPECT_EQ(model_.get_state("nonexistent"), ClusterNodeState::Removed);
}

TEST_F(ClusterFailureModelTest, TransitionOnUnknownNodeFails) {
    auto result = model_.transition("nonexistent", ClusterNodeState::Alive, "test");
    EXPECT_FALSE(result.success);
}

TEST_F(ClusterFailureModelTest, DefaultPolicyIsFailOpen) {
    EXPECT_EQ(model_.get_partition_policy(), PartitionPolicy::FailOpen);
}

TEST_F(ClusterFailureModelTest, SetPartitionPolicy) {
    model_.set_partition_policy(PartitionPolicy::FailClosed);
    EXPECT_EQ(model_.get_partition_policy(), PartitionPolicy::FailClosed);
}

TEST(RouteInvalidationTest, InvalidateTriggersOnDown) {
    EXPECT_TRUE(should_invalidate_routes(ClusterNodeState::Down));
}

TEST(RouteInvalidationTest, InvalidateTriggersOnQuarantined) {
    EXPECT_TRUE(should_invalidate_routes(ClusterNodeState::Quarantined));
}

TEST(RouteInvalidationTest, InvalidateTriggersOnRemoved) {
    EXPECT_TRUE(should_invalidate_routes(ClusterNodeState::Removed));
}

TEST(RouteInvalidationTest, NoInvalidateOnAlive) {
    EXPECT_FALSE(should_invalidate_routes(ClusterNodeState::Alive));
}

TEST(RouteInvalidationTest, NoInvalidateOnSuspect) {
    EXPECT_FALSE(should_invalidate_routes(ClusterNodeState::Suspect));
}

TEST(RouteInvalidationTest, NoInvalidateOnJoining) {
    EXPECT_FALSE(should_invalidate_routes(ClusterNodeState::Joining));
}

TEST(RouteInvalidationTest, NoInvalidateOnLeaving) {
    EXPECT_FALSE(should_invalidate_routes(ClusterNodeState::Leaving));
}

TEST(RouteInvalidationTest, NoInvalidateOnUnreachable) {
    EXPECT_FALSE(should_invalidate_routes(ClusterNodeState::Unreachable));
}

} // namespace hpactor::cluster
