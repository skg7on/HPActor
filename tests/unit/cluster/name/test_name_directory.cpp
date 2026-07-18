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

#include <hpactor/cluster/name/name_directory.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor::cluster::name {
namespace {

// Helper: build a NameEntry for testing.
NameEntry make_entry(uint64_t actor_id_val, const std::string& ep_str,
                     uint64_t gen = 1) {
    NameEntry e;
    e.actor_id = ActorId{actor_id_val};
    e.endpoint = endpoint_ops::parse_endpoint(ep_str);
    e.generation = gen;
    e.registered_at = std::chrono::steady_clock::now();
    return e;
}

// ── Construction & empty state ─────────────────────────────────────────

TEST(NameDirectoryTest, EmptyOnConstruction) {
    NameDirectory dir;
    EXPECT_EQ(dir.size(), 0u);
    EXPECT_FALSE(dir.resolve("anything").has_value());
}

// ── Register & resolve ─────────────────────────────────────────────────

TEST(NameDirectoryTest, RegisterAndResolve) {
    NameDirectory dir;
    auto entry = make_entry(42, "192.168.1.1:9000");

    auto result = dir.register_entry("billing", entry);
    EXPECT_EQ(result, RegisterResult::Ok);
    EXPECT_EQ(dir.size(), 1u);

    auto resolved = dir.resolve("billing");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->actor_id.value(), 42u);
    EXPECT_EQ(resolved->generation, 1u);
}

TEST(NameDirectoryTest, ResolveNotFound) {
    NameDirectory dir;
    EXPECT_FALSE(dir.resolve("nonexistent").has_value());
}

// ── Duplicate name rejection ───────────────────────────────────────────

TEST(NameDirectoryTest, RejectDuplicateName) {
    NameDirectory dir;
    auto e1 = make_entry(1, "192.168.1.1:9000");
    auto e2 = make_entry(2, "192.168.1.2:9000");

    EXPECT_EQ(dir.register_entry("svc", e1), RegisterResult::Ok);
    EXPECT_EQ(dir.register_entry("svc", e2), RegisterResult::DuplicateName);
    EXPECT_EQ(dir.size(), 1u);

    // First registration still intact.
    auto resolved = dir.resolve("svc");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->actor_id.value(), 1u);
}

// ── Unregister ─────────────────────────────────────────────────────────

TEST(NameDirectoryTest, UnregisterExisting) {
    NameDirectory dir;
    dir.register_entry("svc", make_entry(1, "192.168.1.1:9000"));
    EXPECT_TRUE(dir.unregister("svc"));
    EXPECT_EQ(dir.size(), 0u);
    EXPECT_FALSE(dir.resolve("svc").has_value());
}

TEST(NameDirectoryTest, UnregisterUnknown) {
    NameDirectory dir;
    EXPECT_FALSE(dir.unregister("nope"));
}

// ── Generation guard ───────────────────────────────────────────────────

TEST(NameDirectoryTest, GenerationGuardRejectsStale) {
    NameDirectory dir;
    // Register with gen=5.
    auto e5 = make_entry(1, "192.168.1.1:9000", 5);
    EXPECT_EQ(dir.register_entry("svc", e5), RegisterResult::Ok);

    // Stale gen=3 should be rejected.
    auto e3 = make_entry(2, "192.168.1.2:9000", 3);
    EXPECT_EQ(dir.register_entry("svc", e3), RegisterResult::StaleGeneration);

    // Same gen=5 — different actor trying to claim same name => duplicate.
    auto e5b = make_entry(2, "192.168.1.2:9000", 5);
    EXPECT_EQ(dir.register_entry("svc", e5b), RegisterResult::DuplicateName);

    // Higher gen=6 should succeed (re-registration).
    auto e6 = make_entry(3, "192.168.1.3:9000", 6);
    EXPECT_EQ(dir.register_entry("svc", e6), RegisterResult::Ok);

    auto resolved = dir.resolve("svc");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->actor_id.value(), 3u);
    EXPECT_EQ(resolved->generation, 6u);
}

// ── purge_by_endpoint ──────────────────────────────────────────────────

TEST(NameDirectoryTest, PurgeByEndpoint) {
    NameDirectory dir;
    auto ep_a = endpoint_ops::parse_endpoint("192.168.1.1:9000");

    auto e1 = make_entry(1, "192.168.1.1:9000");
    auto e2 = make_entry(2, "192.168.1.2:9000");
    auto e3 = make_entry(3, "192.168.1.1:9000");

    dir.register_entry("a", e1);
    dir.register_entry("b", e2);
    dir.register_entry("c", e3);

    // Purge endpoint A: removes "a" and "c", keeps "b".
    size_t purged = dir.purge_by_endpoint(ep_a);
    EXPECT_EQ(purged, 2u);
    EXPECT_EQ(dir.size(), 1u);
    EXPECT_FALSE(dir.resolve("a").has_value());
    EXPECT_TRUE(dir.resolve("b").has_value());
    EXPECT_FALSE(dir.resolve("c").has_value());
}

// ── Snapshot ───────────────────────────────────────────────────────────

TEST(NameDirectoryTest, SnapshotReturnsAllEntries) {
    NameDirectory dir;
    dir.register_entry("a", make_entry(1, "192.168.1.1:9000"));
    dir.register_entry("b", make_entry(2, "192.168.1.2:9000"));

    auto snap = dir.snapshot();
    EXPECT_EQ(snap.size(), 2u);
}

// ── Thread safety: concurrent register + resolve ───────────────────────

TEST(NameDirectoryTest, ConcurrentRegisterResolve) {
    NameDirectory dir;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> ok_count{0};
    std::atomic<int> dup_count{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&dir, t, &ok_count, &dup_count]() {
            for (int i = 0; i < kPerThread; ++i) {
                auto name = "name_" + std::to_string(t) + "_" + std::to_string(i);
                auto ep_str = "192.168.1." + std::to_string(t + 1) + ":9000";
                auto entry = make_entry(static_cast<uint64_t>(t * kPerThread + i),
                                        ep_str);
                auto result = dir.register_entry(name, entry);
                if (result == RegisterResult::Ok) ok_count++;
                else if (result == RegisterResult::DuplicateName) dup_count++;
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_GT(ok_count.load(), 0);
    EXPECT_EQ(dir.size(), static_cast<size_t>(ok_count.load()));
}

} // namespace
} // namespace hpactor::cluster::name
