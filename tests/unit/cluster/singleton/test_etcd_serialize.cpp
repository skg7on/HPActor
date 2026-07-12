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

#include <hpactor/cluster/singleton/leadership_lease.hpp>
#include <hpactor/etcd/etcd_serialize.hpp>

namespace hpactor::cluster::singleton {

TEST(EtcdSerializeTest, RoundTripPreservesFields) {
    LeadershipLease original;
    original.cluster_id = "test-cluster";
    original.singleton_name = "shard-coordinator";
    original.owner_node_id = "node-a";
    original.owner_incarnation = 42;
    original.owner_process_start_id = 1;
    original.membership_epoch = 7;
    original.fencing_token = 100;
    original.backend_term = 3;
    original.backend_revision = 100;

    auto json = etcd::serialize_lease(original);
    ASSERT_FALSE(json.empty());

    auto restored = etcd::deserialize_lease(json);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->cluster_id, "test-cluster");
    EXPECT_EQ(restored->singleton_name, "shard-coordinator");
    EXPECT_EQ(restored->owner_node_id, "node-a");
    EXPECT_EQ(restored->owner_incarnation, 42u);
    EXPECT_EQ(restored->owner_process_start_id, 1u);
    EXPECT_EQ(restored->membership_epoch, 7u);
    EXPECT_EQ(restored->fencing_token, 100u);
    EXPECT_EQ(restored->backend_term, 3u);
    EXPECT_EQ(restored->backend_revision, 100u);
}

TEST(EtcdSerializeTest, EmptyDataReturnsNullopt) {
    auto result = etcd::deserialize_lease("");
    EXPECT_FALSE(result.has_value());
}

TEST(EtcdSerializeTest, CorruptDataReturnsNullopt) {
    auto result = etcd::deserialize_lease("{not-valid-json");
    EXPECT_FALSE(result.has_value());
}

TEST(EtcdSerializeTest, OwnerKeyFormat) {
    auto key = etcd::owner_key("/hpactor", "prod-a", "shard-coordinator");
    EXPECT_EQ(key, "/hpactor/prod-a/singletons/shard-coordinator/owner");
}

} // namespace hpactor::cluster::singleton
