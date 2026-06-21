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
#include <hpactor/cluster/cluster_node_identity.hpp>

namespace hpactor::cluster {

TEST(ClusterNodeIdentityTest, DefaultConstruction) {
    ClusterNodeIdentity id;
    EXPECT_EQ(id.incarnation, 0);
    EXPECT_EQ(id.process_start_id, 0);
    EXPECT_EQ(id.membership_epoch, 0);
}

TEST(ClusterNodeIdentityTest, HigherIncarnationFencesOlder) {
    ClusterNodeIdentity old_id;
    old_id.node_id = "node-1";
    old_id.incarnation = 1;
    old_id.process_start_id = 100;

    ClusterNodeIdentity new_id;
    new_id.node_id = "node-1";
    new_id.incarnation = 2;
    new_id.process_start_id = 200;

    EXPECT_TRUE(fences(new_id, old_id));
    EXPECT_FALSE(fences(old_id, new_id));
}

TEST(ClusterNodeIdentityTest, DifferentNodesCannotFence) {
    ClusterNodeIdentity node_a;
    node_a.node_id = "node-a";
    node_a.incarnation = 5;

    ClusterNodeIdentity node_b;
    node_b.node_id = "node-b";
    node_b.incarnation = 1;

    EXPECT_FALSE(fences(node_a, node_b));
    EXPECT_FALSE(fences(node_b, node_a));
}

TEST(ClusterNodeIdentityTest, SameIncarnationDifferentProcessStartIsConflict) {
    ClusterNodeIdentity a;
    a.node_id = "node-1";
    a.incarnation = 3;
    a.process_start_id = 100;

    ClusterNodeIdentity b;
    b.node_id = "node-1";
    b.incarnation = 3;
    b.process_start_id = 200;

    EXPECT_TRUE(is_identity_conflict(a, b));
}

TEST(ClusterNodeIdentityTest, SameIdentityNoConflict) {
    ClusterNodeIdentity a;
    a.node_id = "node-1";
    a.incarnation = 3;
    a.process_start_id = 100;

    ClusterNodeIdentity b = a;

    EXPECT_FALSE(is_identity_conflict(a, b));
}

TEST(ClusterNodeIdentityTest, DifferentClusterIdRejected) {
    ClusterNodeIdentity local;
    local.node_id = "node-1";
    local.cluster_id = "prod-us-east";

    ClusterNodeIdentity remote;
    remote.node_id = "node-2";
    remote.cluster_id = "prod-us-west";

    EXPECT_FALSE(same_cluster(local, remote));
}

TEST(ClusterNodeIdentityTest, SameClusterIdAccepted) {
    ClusterNodeIdentity local;
    local.node_id = "node-1";
    local.cluster_id = "prod-us-east";

    ClusterNodeIdentity remote;
    remote.node_id = "node-2";
    remote.cluster_id = "prod-us-east";

    EXPECT_TRUE(same_cluster(local, remote));
}

TEST(ClusterNodeIdentityTest, StaleEpochDetected) {
    ClusterNodeIdentity current;
    current.membership_epoch = 10;

    ClusterNodeIdentity stale;
    stale.membership_epoch = 5;

    EXPECT_TRUE(has_stale_epoch(stale, current));
    EXPECT_FALSE(has_stale_epoch(current, stale));
    EXPECT_FALSE(has_stale_epoch(current, current));
}

} // namespace hpactor::cluster
