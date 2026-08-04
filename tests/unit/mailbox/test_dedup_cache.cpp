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
#include <hpactor/adt/dedup_cache.hpp>
#include <hpactor/ref/actor_address.hpp>

#include <thread>
#include <vector>

using namespace hpactor;
using namespace hpactor::adt;

namespace {

// Helper to create a localhost IPv4 endpoint
EndPoint make_test_endpoint(uint16_t port = 9000) {
    Ipv4Endpoint ep{0x7F000001, port}; // 127.0.0.1 in network byte order
    return EndPoint{ep};
}

} // anonymous namespace

TEST(DedupCacheTest, InsertNotDuplicate) {
    DedupCache::Config cfg;
    cfg.max_entries = 1024;
    DedupCache cache(cfg);

    EndPoint ep = make_test_endpoint();
    EXPECT_FALSE(cache.is_duplicate(ep, ActorId{1}, MessageId{100}));
    EXPECT_EQ(cache.insertions(), 1);
    EXPECT_EQ(cache.duplicate_hits(), 0);
}

TEST(DedupCacheTest, DuplicateDetected) {
    DedupCache::Config cfg;
    DedupCache cache(cfg);

    EndPoint ep = make_test_endpoint();
    EXPECT_FALSE(cache.is_duplicate(ep, ActorId{1}, MessageId{100}));
    EXPECT_TRUE(cache.is_duplicate(ep, ActorId{1}, MessageId{100}));
    EXPECT_EQ(cache.duplicate_hits(), 1);
}

TEST(DedupCacheTest, DifferentKeysNotDuplicate) {
    DedupCache::Config cfg;
    DedupCache cache(cfg);

    EndPoint ep = make_test_endpoint();
    EXPECT_FALSE(cache.is_duplicate(ep, ActorId{1}, MessageId{100}));
    EXPECT_FALSE(cache.is_duplicate(ep, ActorId{1}, MessageId{200}));
    EXPECT_FALSE(cache.is_duplicate(ep, ActorId{2}, MessageId{100}));
    EXPECT_EQ(cache.insertions(), 3);
}

TEST(DedupCacheTest, DifferentEndpointsNotDuplicate) {
    DedupCache::Config cfg;
    DedupCache cache(cfg);

    EndPoint ep1 = make_test_endpoint(9000);
    EndPoint ep2 = make_test_endpoint(9001);
    EXPECT_FALSE(cache.is_duplicate(ep1, ActorId{1}, MessageId{100}));
    EXPECT_FALSE(cache.is_duplicate(ep2, ActorId{1}, MessageId{100}));
    EXPECT_EQ(cache.insertions(), 2);
}

TEST(DedupCacheTest, SizeIncrementsOnInsert) {
    DedupCache::Config cfg;
    DedupCache cache(cfg);

    EndPoint ep = make_test_endpoint();
    EXPECT_EQ(cache.size(), 0);
    (void)cache.is_duplicate(ep, ActorId{1}, MessageId{1});
    EXPECT_EQ(cache.size(), 1);
    (void)cache.is_duplicate(ep, ActorId{1}, MessageId{2});
    EXPECT_EQ(cache.size(), 2);
    // Duplicate shouldn't increase size
    (void)cache.is_duplicate(ep, ActorId{1}, MessageId{1});
    EXPECT_EQ(cache.size(), 2);
}

TEST(DedupCacheTest, Counters) {
    DedupCache::Config cfg;
    DedupCache cache(cfg);

    EndPoint ep = make_test_endpoint();
    (void)cache.is_duplicate(ep, ActorId{1}, MessageId{100});
    EXPECT_EQ(cache.insertions(), 1);
    EXPECT_EQ(cache.duplicate_hits(), 0);

    (void)cache.is_duplicate(ep, ActorId{1}, MessageId{100});
    EXPECT_EQ(cache.insertions(), 1);
    EXPECT_EQ(cache.duplicate_hits(), 1);

    (void)cache.is_duplicate(ep, ActorId{1}, MessageId{200});
    EXPECT_EQ(cache.insertions(), 2);
    EXPECT_EQ(cache.duplicate_hits(), 1);
}

TEST(DedupCacheTest, PurgeRemovesExpired) {
    // Use a zero TTL so every entry is immediately expired after a
    // purge that assigns timestamps.
    DedupCache::Config cfg;
    cfg.ttl_ns = 1;
    DedupCache cache(cfg);

    EndPoint ep = make_test_endpoint();
    EXPECT_FALSE(cache.is_duplicate(ep, ActorId{1}, MessageId{100}));
    EXPECT_EQ(cache.size(), 1);

    // First purge stamps the entry with now_ns.
    cache.purge_expired(1000);
    // Second purge with now_ns beyond TTL removes it.
    cache.purge_expired(1000 + 2);
    EXPECT_EQ(cache.size(), 0);

    // After expiry, re-inserting the same key is not a duplicate.
    EXPECT_FALSE(cache.is_duplicate(ep, ActorId{1}, MessageId{100}));
    EXPECT_EQ(cache.size(), 1);
}

TEST(DedupCacheTest, PurgePreservesUnexpired) {
    DedupCache::Config cfg;
    cfg.ttl_ns = 100'000'000; // 100ms TTL
    DedupCache cache(cfg);

    EndPoint ep = make_test_endpoint();
    EXPECT_FALSE(cache.is_duplicate(ep, ActorId{1}, MessageId{100}));

    // Purge at t=1000 stamps entries, then purge at t=5000 within TTL.
    cache.purge_expired(1000);
    cache.purge_expired(5000);
    EXPECT_EQ(cache.size(), 1);

    // Entry still present → should be detected as duplicate.
    EXPECT_TRUE(cache.is_duplicate(ep, ActorId{1}, MessageId{100}));
}

TEST(DedupCacheTest, MoveSemantics) {
    DedupCache::Config cfg;
    cfg.max_entries = 100;
    DedupCache cache1(cfg);

    EndPoint ep = make_test_endpoint();
    (void)cache1.is_duplicate(ep, ActorId{1}, MessageId{100});
    EXPECT_EQ(cache1.size(), 1);

    DedupCache cache2(std::move(cache1));
    EXPECT_EQ(cache2.size(), 1);
    EXPECT_TRUE(cache2.is_duplicate(ep, ActorId{1}, MessageId{100}));
}

TEST(DedupCacheTest, ConcurrentAccess) {
    DedupCache::Config cfg;
    cfg.max_entries = 10000;
    DedupCache cache(cfg);

    EndPoint ep = make_test_endpoint();
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, &ep, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                // Each thread uses a unique actor_id range to avoid too many
                // collisions (we're testing safety, not dedup logic)
                (void)cache.is_duplicate(
                    ep, ActorId{static_cast<uint64_t>(t * kPerThread + i + 1)},
                    MessageId{static_cast<uint64_t>(i)});
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // All inserts should be non-duplicates (unique actor_id per thread)
    EXPECT_EQ(cache.insertions(), kThreads * kPerThread);
    EXPECT_EQ(cache.size(), kThreads * kPerThread);
}

TEST(DedupCacheTest, RemoveRollsBackInsertion) {
    // MSG-006: Verifies that remove() correctly rolls back an insertion
    // so that a retry after a rejected delivery is not suppressed.
    DedupCache::Config cfg;
    DedupCache cache(cfg);

    EndPoint ep = make_test_endpoint();
    ActorId actor{1};
    MessageId msg_id{42};

    // Insert via is_duplicate → returns false (new entry).
    EXPECT_FALSE(cache.is_duplicate(ep, actor, msg_id));
    EXPECT_EQ(cache.size(), 1);

    // Roll back the insertion.
    cache.remove(ep, actor, msg_id);
    EXPECT_EQ(cache.size(), 0);

    // After removal, re-inserting the same key should be a new insertion.
    EXPECT_FALSE(cache.is_duplicate(ep, actor, msg_id));
    EXPECT_EQ(cache.size(), 1);
    EXPECT_EQ(cache.insertions(), 2); // both were non-duplicates
}

TEST(DedupCacheTest, RemoveNonExistentIsNoop) {
    DedupCache::Config cfg;
    DedupCache cache(cfg);

    EndPoint ep = make_test_endpoint();
    // Removing a key that was never inserted should be a safe no-op.
    cache.remove(ep, ActorId{1}, MessageId{99});
    EXPECT_EQ(cache.size(), 0);
}
