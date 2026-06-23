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
#include <hpactor/actor/receptionist/receptionist_messages.hpp>
#include <hpactor/cluster/receptionist/cluster_receptionist_core.hpp>

namespace hpactor::cluster::receptionist {

using hpactor::receptionist::ServiceKey;

TEST(ClusterReceptionistCoreTest, ApplyLocalRegistrationAddsKey) {
    ClusterReceptionistCore core;
    ServiceKey key{"worker"};
    core.apply_local_registration(key, {100, 200});
    EXPECT_EQ(core.local_key_count(), 1);
}

TEST(ClusterReceptionistCoreTest, GetClusterListingIncludesLocal) {
    ClusterReceptionistCore core;
    ServiceKey key{"worker"};
    core.apply_local_registration(key, {10, 20});
    auto listing = core.get_cluster_listing(key);
    EXPECT_EQ(listing.size(), 2);
}

TEST(ClusterReceptionistCoreTest, MergeRemoteRegistrationAddsRemote) {
    ClusterReceptionistCore core;
    ServiceKey key{"db"};
    ClusterRegistration reg{key, "node-b", {300, 301}, 1};
    core.merge_remote_registration(reg);
    EXPECT_EQ(core.remote_key_count(), 1);
}

TEST(ClusterReceptionistCoreTest, GetClusterListingCombinesLocalAndRemote) {
    ClusterReceptionistCore core;
    ServiceKey key{"router"};
    core.apply_local_registration(key, {1});
    core.merge_remote_registration(ClusterRegistration{key, "node-x", {999}, 1});
    auto listing = core.get_cluster_listing(key);
    EXPECT_EQ(listing.size(), 2); // 1 local + 1 remote
}

TEST(ClusterReceptionistCoreTest, HasKeyChecksLocalAndRemote) {
    ClusterReceptionistCore core;
    EXPECT_FALSE(core.has_key(ServiceKey{"nonexistent"}));
    core.apply_local_registration(ServiceKey{"local-key"}, {1});
    EXPECT_TRUE(core.has_key(ServiceKey{"local-key"}));
    core.merge_remote_registration(
        ClusterRegistration{ServiceKey{"remote-key"}, "node-y", {2}, 0});
    EXPECT_TRUE(core.has_key(ServiceKey{"remote-key"}));
}

TEST(ClusterReceptionistCoreTest, RemoveNodeRegistrationsClearsRemote) {
    ClusterReceptionistCore core;
    ServiceKey key{"ephemeral"};
    core.merge_remote_registration(ClusterRegistration{key, "node-a", {1}, 0});
    core.merge_remote_registration(ClusterRegistration{key, "node-b", {2}, 0});
    EXPECT_EQ(core.remote_key_count(), 1); // 1 key across 2 nodes

    core.remove_node_registrations("node-a");
    auto listing = core.get_cluster_listing(key);
    EXPECT_EQ(listing.size(), 1);
    EXPECT_EQ(listing[0], 2); // only node-b's actor remains
}

TEST(ClusterReceptionistCoreTest, HigherIncarnationOverwritesRemote) {
    ClusterReceptionistCore core;
    ServiceKey key{"solo"};
    // First registration with incarnation 0
    core.merge_remote_registration(ClusterRegistration{key, "node-a", {100}, 0});
    // Second registration with higher incarnation — should overwrite
    core.merge_remote_registration(ClusterRegistration{key, "node-a", {200}, 1});

    auto listing = core.get_cluster_listing(key);
    EXPECT_EQ(listing.size(), 1);
    EXPECT_EQ(listing[0], 200) << "higher incarnation should overwrite previous";
}

TEST(ClusterReceptionistCoreTest, MultipleKeysAreIndependent) {
    ClusterReceptionistCore core;
    core.apply_local_registration(ServiceKey{"key-a"}, {1});
    core.apply_local_registration(ServiceKey{"key-b"}, {2});
    EXPECT_EQ(core.local_key_count(), 2);
    EXPECT_TRUE(core.has_key(ServiceKey{"key-a"}));
    EXPECT_TRUE(core.has_key(ServiceKey{"key-b"}));
}

TEST(ClusterReceptionistCoreTest, DrainDirtyRegistrationsCapturesNewRegs) {
    ClusterReceptionistCore core;
    core.apply_local_registration(ServiceKey{"dirty"}, {42});
    auto dirty = core.drain_dirty_registrations();
    EXPECT_GE(dirty.size(), 1);
}

TEST(ClusterReceptionistCoreTest, DrainClearsAfterRead) {
    ClusterReceptionistCore core;
    core.apply_local_registration(ServiceKey{"flush"}, {1});
    core.drain_dirty_registrations();
    auto second = core.drain_dirty_registrations();
    EXPECT_TRUE(second.empty());
}

} // namespace hpactor::cluster::receptionist
