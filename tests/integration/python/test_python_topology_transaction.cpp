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

#include <hpactor/config/topology_model.hpp>
#include <hpactor/runtime/configured_actor_provider.hpp>
#include <hpactor/runtime/topology_bootstrap_transaction.hpp>
#include <hpactor/actor/system/actor_directory.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace hpactor;

namespace {

// ── Test provider that uses a real ActorDirectory for lease tracking ────────

struct TestJournal {
    std::mutex mutex;
    std::vector<size_t> spawn_order;
    std::vector<size_t> ready_order;
    std::vector<size_t> rollback_order;
    size_t commit_count{0};
};

struct TestProviderCtx {
    ActorDirectory* directory{nullptr};
    TestJournal* journal{nullptr};
    size_t fail_at{0};  // 0 = never fail
    std::atomic<size_t> call_count{0};
};

bool test_matches(void* ctx, const ConfiguredActorPlan& plan) noexcept {
    (void)ctx;
    return plan.provider == ConfiguredActorProviderKind::External;
}

result<ActorSpawnLease> test_spawn(void* ctx, const config::ActorDef& def,
                                    const ConfiguredActorPlan& plan) noexcept {
    auto* tc = static_cast<TestProviderCtx*>(ctx);
    (void)def;
    size_t n = tc->call_count.fetch_add(1) + 1;

    {
        std::lock_guard<std::mutex> lock(tc->journal->mutex);
        tc->journal->spawn_order.push_back(plan.topology_index);
    }

    if (tc->fail_at > 0 && n >= tc->fail_at) {
        return result<ActorSpawnLease>::make(
            error(errors::unknown, "injected spawn failure"));
    }

    return result<ActorSpawnLease>::make(ActorSpawnLease{});
}

result<void> test_ready(void* ctx, ActorId id, const ConfiguredActorPlan& plan,
                        std::chrono::milliseconds timeout) noexcept {
    auto* tc = static_cast<TestProviderCtx*>(ctx);
    (void)id; (void)timeout;

    {
        std::lock_guard<std::mutex> lock(tc->journal->mutex);
        tc->journal->ready_order.push_back(plan.topology_index);
    }

    if (tc->fail_at > 0) {
        size_t n = tc->call_count.load();
        if (n >= tc->fail_at) {
            return result<void>::make(
                error(errors::unknown, "injected ready failure"));
        }
    }

    return result<void>::make();
}

result<void> test_rollback(void* ctx, ActorId id,
                            const ConfiguredActorPlan& plan) noexcept {
    auto* tc = static_cast<TestProviderCtx*>(ctx);
    (void)id;
    {
        std::lock_guard<std::mutex> lock(tc->journal->mutex);
        tc->journal->rollback_order.push_back(plan.topology_index);
    }
    return result<void>::make();
}

ConfiguredActorProviderPort make_test_port(TestProviderCtx* ctx) {
    ConfiguredActorProviderPort p;
    p.context = ctx;
    p.matches = test_matches;
    p.spawn_unpublished = test_spawn;
    p.await_ready = test_ready;
    p.rollback_actor = test_rollback;
    return p;
}

} // namespace

// ── Integration tests ──────────────────────────────────────────────────────

TEST(PythonTopologyTransactionTest, EmptyTopologySucceeds) {
    config::TopologyModel model;
    ActorDirectory directory;

    ConfiguredActorProviderPort empty_cpp;
    ConfiguredActorProviderPort empty_ext;

    auto result = TopologyBootstrapTransaction::execute(
        model, {}, 0, empty_cpp, empty_ext, directory,
        std::chrono::milliseconds(100));
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(result.value().actor_count, 0u);
}

TEST(PythonTopologyTransactionTest, SpawnAndReadyOrderFollowsModel) {
    // Verify that spawn_unpublished and await_ready are called in model order.
    config::TopologyModel model;
    config::ActorDef def_a, def_b;
    def_a.id = "a"; def_a.behavior = "python:pkg:A";
    def_b.id = "b"; def_b.behavior = "python:pkg:B";
    model.actors = {def_a, def_b};

    ActorDirectory directory;
    TestJournal journal;
    TestProviderCtx ctx{&directory, &journal, /*fail_at=*/3}; // fail on 3rd call

    std::array<ConfiguredActorPlan, 2> specs{{
        {0, ConfiguredActorProviderKind::External, 100, 0},
        {1, ConfiguredActorProviderKind::External, 200, 0},
    }};

    auto port = make_test_port(&ctx);
    ConfiguredActorProviderPort empty_cpp;

    auto result = TopologyBootstrapTransaction::execute(
        model, specs, 0xABCD, empty_cpp, port, directory,
        std::chrono::milliseconds(1000));
    // Expect failure on 3rd call (2nd ready).
    EXPECT_FALSE(result.ok());

    // Spawn order follows model order: [0, 1].
    EXPECT_EQ(journal.spawn_order, (std::vector<size_t>{0, 1}));
    // Ready order follows model order: [0, 1].
    EXPECT_EQ(journal.ready_order, (std::vector<size_t>{0, 1}));
    // Rollback must be reverse: [1, 0].
    EXPECT_EQ(journal.rollback_order, (std::vector<size_t>{1, 0}));
}

TEST(PythonTopologyTransactionTest, SpawnFailureTriggersRollback) {
    config::TopologyModel model;
    config::ActorDef def_a, def_b, def_c;
    def_a.id = "a"; def_a.behavior = "python:pkg:A";
    def_b.id = "b"; def_b.behavior = "python:pkg:B";
    def_c.id = "c"; def_c.behavior = "python:pkg:C";
    model.actors = {def_a, def_b, def_c};

    ActorDirectory directory;
    TestJournal journal;
    TestProviderCtx ctx{&directory, &journal, /*fail_at=*/2}; // fail on 2nd spawn

    std::array<ConfiguredActorPlan, 3> specs{{
        {0, ConfiguredActorProviderKind::External, 100, 0},
        {1, ConfiguredActorProviderKind::External, 200, 0},
        {2, ConfiguredActorProviderKind::External, 300, 0},
    }};

    auto port = make_test_port(&ctx);
    ConfiguredActorProviderPort empty_cpp;

    auto result = TopologyBootstrapTransaction::execute(
        model, specs, 0, empty_cpp, port, directory,
        std::chrono::milliseconds(1000));
    EXPECT_FALSE(result.ok());

    // First actor was spawned and should be rolled back.
    EXPECT_GE(journal.rollback_order.size(), 1u);
    // Rollback is reverse order.
    if (!journal.rollback_order.empty()) {
        EXPECT_EQ(journal.rollback_order[0], 0u);
    }
}
