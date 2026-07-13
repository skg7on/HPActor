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

#include <hpactor/cluster/name/consistent_hash_ring.hpp>
#include <hpactor/ref/actor_address.hpp>

#include <set>
#include <unordered_map>

namespace hpactor::cluster::name {
namespace {

using EndPointSet = std::set<EndPoint, EndPointCompare>;

EndPoint ep(const std::string& s) { return endpoint_ops::parse_endpoint(s); }

// ── Construction & empty state ─────────────────────────────────────────

TEST(ConsistentHashRingTest, EmptyOnConstruction) {
    ConsistentHashRing ring;
    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(ring.size(), 0u);
}

TEST(ConsistentHashRingTest, LookupOnEmptyReturnsNullopt) {
    ConsistentHashRing ring;
    EXPECT_FALSE(ring.lookup("anything").has_value());
}

// ── Single node ────────────────────────────────────────────────────────

TEST(ConsistentHashRingTest, SingleNodeAlwaysReturnsIt) {
    ConsistentHashRing ring;
    EndPointSet members = {ep("192.168.1.1:9000")};
    ring.build(members, 100);
    EXPECT_FALSE(ring.empty());
    EXPECT_EQ(ring.size(), 1u);

    auto result = ring.lookup("billing");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, ep("192.168.1.1:9000"));

    // Same node for any name.
    auto result2 = ring.lookup("totally_different_name");
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(*result2, ep("192.168.1.1:9000"));
}

// ── Determinism ────────────────────────────────────────────────────────

TEST(ConsistentHashRingTest, SameMembersProduceSameMapping) {
    EndPointSet members = {
        ep("192.168.1.1:9000"),
        ep("192.168.1.2:9000"),
        ep("192.168.1.3:9000"),
    };

    ConsistentHashRing ring1, ring2;
    ring1.build(members, 100);
    ring2.build(members, 100);

    for (const auto& name : {"a", "b", "c", "svc1", "svc2", "worker"}) {
        EXPECT_EQ(ring1.lookup(name), ring2.lookup(name))
            << "Mismatch for name: " << name;
    }
}

// ── Distribution: virtual nodes provide balanced mapping ───────────────

TEST(ConsistentHashRingTest, DistributionIsRoughlyBalanced) {
    EndPointSet members = {
        ep("10.0.0.1:9000"),
        ep("10.0.0.2:9000"),
        ep("10.0.0.3:9000"),
    };

    ConsistentHashRing ring;
    ring.build(members, 200);

    std::unordered_map<EndPoint, int> counts;
    constexpr int kNames = 10000;
    for (int i = 0; i < kNames; ++i) {
        auto result = ring.lookup("name_" + std::to_string(i));
        ASSERT_TRUE(result.has_value());
        counts[*result]++;
    }

    // With 200 virtual nodes and 10000 names, each physical node should get
    // roughly 33%. Allow generous tolerance (±50% of the expected share).
    int expected = kNames / 3;
    for (const auto& [ep_node, count] : counts) {
        EXPECT_GE(count, expected / 2) << "Node severely under-represented";
        EXPECT_LE(count, expected * 2) << "Node severely over-represented";
    }
}

// ── Add/remove nodes ───────────────────────────────────────────────────

TEST(ConsistentHashRingTest, RebuildAfterNodeAdded) {
    EndPointSet members = {ep("10.0.0.1:9000")};
    ConsistentHashRing ring;
    ring.build(members, 100);

    auto before = ring.lookup("svc");

    // Add a second node.
    members.insert(ep("10.0.0.2:9000"));
    ring.build(members, 100);

    // After rebuild, some names should move to the new node.
    bool any_changed = false;
    for (const auto& name : {"a", "b", "c", "svc", "svc2"}) {
        if (ring.lookup(name) != before) {
            any_changed = true;
            break;
        }
    }
    EXPECT_TRUE(any_changed) << "Adding a node should reassign some names";
}

TEST(ConsistentHashRingTest, RebuildAfterNodeRemoved) {
    EndPointSet members = {
        ep("10.0.0.1:9000"),
        ep("10.0.0.2:9000"),
    };
    ConsistentHashRing ring;
    ring.build(members, 100);

    // Remove node 2.
    members.erase(ep("10.0.0.2:9000"));
    ring.build(members, 100);

    // All lookups must return the surviving node.
    for (const auto& name : {"a", "b", "c", "svc", "svc2"}) {
        auto result = ring.lookup(name);
        ASSERT_TRUE(result.has_value());
        EXPECT_NE(*result, ep("10.0.0.2:9000"))
            << "Removed node should not be returned for " << name;
    }
}

// ── Virtual node count ─────────────────────────────────────────────────

TEST(ConsistentHashRingTest, CustomVirtualNodeCount) {
    EndPointSet members = {ep("10.0.0.1:9000")};
    ConsistentHashRing ring;
    ring.build(members, 1); // single virtual node
    EXPECT_FALSE(ring.empty());
    EXPECT_EQ(ring.size(), 1u);
    EXPECT_TRUE(ring.lookup("anything").has_value());
}

// ── Empty membership clears ring ───────────────────────────────────────

TEST(ConsistentHashRingTest, BuildWithEmptySetClearsRing) {
    EndPointSet members = {ep("10.0.0.1:9000")};
    ConsistentHashRing ring;
    ring.build(members, 100);
    EXPECT_FALSE(ring.empty());

    ring.build({}, 100);
    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.lookup("anything").has_value());
}

} // namespace
} // namespace hpactor::cluster::name
