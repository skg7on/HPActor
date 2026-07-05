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

#include <hpactor/actor/system/actor_ref_cache.hpp>
#include <hpactor/ref/actor_proxy.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

TEST(ActorRefCacheTest, EmptyCache) {
    ActorRefCache cache;
    EXPECT_FALSE(cache.get(ActorId{1}).has_value());
}

TEST(ActorRefCacheTest, PutAndGet) {
    ActorRefCache cache;
    ActorAddress addr{endpoint_ops::parse_endpoint("127.0.0.1:0"), ActorType{1},
                      ActorId{1}, 0};
    ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
    ActorRef ref(std::move(proxy));

    cache.put(ActorId{1}, ref);

    auto result = cache.get(ActorId{1});
    ASSERT_TRUE(result.has_value());
    if (!result.has_value())
        return;
    EXPECT_FALSE(result->is_local());
    EXPECT_EQ(result->address().id, ActorId{1});
}

TEST(ActorRefCacheTest, PutUpdatesExisting) {
    ActorRefCache cache;
    ActorAddress addr1{endpoint_ops::parse_endpoint("127.0.0.1:0"),
                       ActorType{1}, ActorId{1}, 0};
    ActorAddress addr2{endpoint_ops::parse_endpoint("127.0.0.1:0"),
                       ActorType{2}, ActorId{1}, 0};

    ActorProxy proxy1(addr1, static_cast<net::Transport*>(nullptr));
    ActorRef ref1(std::move(proxy1));
    cache.put(ActorId{1}, ref1);

    ActorProxy proxy2(addr2, static_cast<net::Transport*>(nullptr));
    ActorRef ref2(std::move(proxy2));
    cache.put(ActorId{1}, ref2); // overwrite

    auto result = cache.get(ActorId{1});
    ASSERT_TRUE(result.has_value());
    if (!result.has_value())
        return;
    EXPECT_EQ(result->address().type, ActorType{2});
}

TEST(ActorRefCacheTest, EvictionAtMax) {
    ActorRefCache cache(3); // max 3 entries

    for (uint64_t i = 1; i <= 3; ++i) {
        ActorAddress addr{endpoint_ops::parse_endpoint("127.0.0.1:0"),
                          ActorType{1}, ActorId{i}, 0};
        ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
        ActorRef ref(std::move(proxy));
        cache.put(ActorId{i}, ref);
    }

    EXPECT_TRUE(cache.get(ActorId{1}).has_value());
    EXPECT_TRUE(cache.get(ActorId{2}).has_value());
    EXPECT_TRUE(cache.get(ActorId{3}).has_value());

    // Insert 4th — should evict least recently used (id=1)
    ActorAddress addr4{endpoint_ops::parse_endpoint("127.0.0.1:0"),
                       ActorType{1}, ActorId{4}, 0};
    ActorProxy proxy4(addr4, static_cast<net::Transport*>(nullptr));
    ActorRef ref4(std::move(proxy4));
    cache.put(ActorId{4}, ref4);

    EXPECT_FALSE(cache.get(ActorId{1}).has_value());
    EXPECT_TRUE(cache.get(ActorId{2}).has_value());
    EXPECT_TRUE(cache.get(ActorId{3}).has_value());
    EXPECT_TRUE(cache.get(ActorId{4}).has_value());
}

TEST(ActorRefCacheTest, LruAccessUpdatesTick) {
    ActorRefCache cache(3);

    for (uint64_t i = 1; i <= 3; ++i) {
        ActorAddress addr{endpoint_ops::parse_endpoint("127.0.0.1:0"),
                          ActorType{1}, ActorId{i}, 0};
        ActorProxy proxy(addr, static_cast<net::Transport*>(nullptr));
        ActorRef ref(std::move(proxy));
        cache.put(ActorId{i}, ref);
    }

    // Access id=1 (making it recently used), so id=2 becomes LRU
    ASSERT_TRUE(cache.get(ActorId{1}).has_value());

    // Insert 4th — should evict id=2 (now LRU)
    ActorAddress addr4{endpoint_ops::parse_endpoint("127.0.0.1:0"),
                       ActorType{1}, ActorId{4}, 0};
    ActorProxy proxy4(addr4, static_cast<net::Transport*>(nullptr));
    ActorRef ref4(std::move(proxy4));
    cache.put(ActorId{4}, ref4);

    EXPECT_TRUE(cache.get(ActorId{1}).has_value());
    EXPECT_FALSE(cache.get(ActorId{2}).has_value());
    EXPECT_TRUE(cache.get(ActorId{3}).has_value());
    EXPECT_TRUE(cache.get(ActorId{4}).has_value());
}