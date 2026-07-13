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

#include <hpactor/cluster/name/name_resolve_cache.hpp>
#include <hpactor/ref/actor_address.hpp>

namespace hpactor::cluster::name {
namespace {

using namespace std::chrono_literals;

ActorAddress make_addr(const std::string& ep_str, uint64_t id_val) {
    return ActorAddress{endpoint_ops::parse_endpoint(ep_str), ActorType{0},
                        ActorId{id_val}, 0};
}

// ── Construction & empty state ─────────────────────────────────────────

TEST(NameResolveCacheTest, EmptyOnConstruction) {
    NameResolveCache cache;
    EXPECT_FALSE(cache.get("anything").has_value());
}

// ── Put & get ──────────────────────────────────────────────────────────

TEST(NameResolveCacheTest, PutAndGet) {
    NameResolveCache cache;
    auto addr = make_addr("192.168.1.1:9000", 42);
    cache.put("billing", addr, 30s);
    auto result = cache.get("billing");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id.value(), 42u);
}

// ── Miss on unknown name ───────────────────────────────────────────────

TEST(NameResolveCacheTest, GetUnknownReturnsNullopt) {
    NameResolveCache cache;
    EXPECT_FALSE(cache.get("unknown").has_value());
}

// ── TTL expiry ─────────────────────────────────────────────────────────

TEST(NameResolveCacheTest, ExpiredEntryReturnsNullopt) {
    NameResolveCache cache;
    auto addr = make_addr("192.168.1.1:9000", 42);
    // Put with a TTL that has already expired.
    cache.put("billing", addr, -1s);
    EXPECT_FALSE(cache.get("billing").has_value());
}

// ── Evict ──────────────────────────────────────────────────────────────

TEST(NameResolveCacheTest, EvictRemovesEntry) {
    NameResolveCache cache;
    cache.put("a", make_addr("192.168.1.1:9000", 1), 60s);
    cache.put("b", make_addr("192.168.1.2:9000", 2), 60s);
    cache.evict("a");
    EXPECT_FALSE(cache.get("a").has_value());
    EXPECT_TRUE(cache.get("b").has_value());
}

// ── evict_node ─────────────────────────────────────────────────────────

TEST(NameResolveCacheTest, EvictNodeRemovesAllEntriesForEndpoint) {
    NameResolveCache cache;
    auto ep1 = endpoint_ops::parse_endpoint("192.168.1.1:9000");

    cache.put("a", make_addr("192.168.1.1:9000", 1), 60s);
    cache.put("b", make_addr("192.168.1.2:9000", 2), 60s);
    cache.put("c", make_addr("192.168.1.1:9000", 3), 60s);

    cache.evict_node(ep1);
    EXPECT_FALSE(cache.get("a").has_value());
    EXPECT_TRUE(cache.get("b").has_value());
    EXPECT_FALSE(cache.get("c").has_value());
}

// ── purge_expired ──────────────────────────────────────────────────────

TEST(NameResolveCacheTest, PurgeExpiredRemovesOnlyExpired) {
    NameResolveCache cache;
    cache.put("a", make_addr("192.168.1.1:9000", 1), -1s); // expired
    cache.put("b", make_addr("192.168.1.2:9000", 2), 3600s); // fresh
    cache.purge_expired();
    EXPECT_FALSE(cache.get("a").has_value());
    EXPECT_TRUE(cache.get("b").has_value());
}

// ── Overwrite ──────────────────────────────────────────────────────────

TEST(NameResolveCacheTest, PutOverwritesExisting) {
    NameResolveCache cache;
    cache.put("svc", make_addr("192.168.1.1:9000", 1), 60s);
    cache.put("svc", make_addr("192.168.1.2:9000", 2), 60s);
    auto result = cache.get("svc");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->id.value(), 2u);
}

} // namespace
} // namespace hpactor::cluster::name
