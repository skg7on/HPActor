// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <etcd/etcd_serialize.hpp>
#include <hpactor/cluster/singleton/leadership_lease.hpp>

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
