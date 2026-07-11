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

#include <hpactor/actor/system/actor_directory.hpp>
#include <hpactor/runtime/actor_spawn_lease.hpp>
#include <hpactor/runtime/configured_actor_provider.hpp>
#include <hpactor/runtime/topology_bootstrap_transaction.hpp>

#include <condition_variable>
#include <mutex>
#include <thread>

using namespace hpactor;

namespace {

// ── Fake provider for testing ───────────────────────────────────────────────

struct FakeProviderState {
    std::mutex mutex;
    std::condition_variable cv;
    size_t spawn_count{0};
    size_t ready_count{0};
    size_t rollback_count{0};
    bool ready_gate_open{true};
    size_t fail_spawn_at{0};   // 0 = never fail
    size_t fail_ready_at{0};   // 0 = never fail
    std::vector<size_t> rollback_order;
    bool all_ready_received{false};
};

// Matches ALL plans (external provider).
bool fake_matches(void* ctx, const ConfiguredActorPlan& plan) noexcept {
    (void)ctx;
    return plan.provider == ConfiguredActorProviderKind::External;
}

result<ActorSpawnLease> fake_spawn(void* ctx, const config::ActorDef& def,
                                    const ConfiguredActorPlan& plan) noexcept {
    (void)def; (void)plan;
    auto* state = static_cast<FakeProviderState*>(ctx);
    state->spawn_count++;
    if (state->fail_spawn_at > 0 && state->spawn_count >= state->fail_spawn_at) {
        return result<ActorSpawnLease>::make(
            error(errors::unknown, "injected spawn failure"));
    }
    // Return an empty lease (tests don't exercise real actor lifecycle).
    return result<ActorSpawnLease>::make(ActorSpawnLease{});
}

result<void> fake_await_ready(void* ctx, ActorId id,
                               const ConfiguredActorPlan& plan,
                               std::chrono::milliseconds timeout) noexcept {
    (void)id; (void)plan; (void)timeout;
    auto* state = static_cast<FakeProviderState*>(ctx);
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->ready_count++;
        state->all_ready_received =
            !state->cv.wait_for(lock, std::chrono::seconds(1),
                                [&] { return state->ready_gate_open; });
        if (!state->ready_gate_open && state->all_ready_received) {
            // timed out waiting for gate
            return result<void>::make(
                error(errors::timeout, "ready gate closed"));
        }
    }
    if (state->fail_ready_at > 0 && state->ready_count >= state->fail_ready_at) {
        return result<void>::make(
            error(errors::unknown, "injected ready failure"));
    }
    return result<void>::make();
}

result<void> fake_rollback(void* ctx, ActorId id,
                            const ConfiguredActorPlan& plan) noexcept {
    (void)id;
    auto* state = static_cast<FakeProviderState*>(ctx);
    state->rollback_count++;
    state->rollback_order.push_back(plan.topology_index);
    return result<void>::make();
}

ConfiguredActorProviderPort make_fake_port(FakeProviderState* ctx) {
    ConfiguredActorProviderPort port;
    port.context = ctx;
    port.matches = fake_matches;
    port.spawn_unpublished = fake_spawn;
    port.await_ready = fake_await_ready;
    port.rollback_actor = fake_rollback;
    return port;
}

} // namespace

// ── Provider port contracts ─────────────────────────────────────────────────

TEST(TopologyBootstrapTransactionTest, FakeProviderPortCanBeConstructed) {
    FakeProviderState state;
    auto port = make_fake_port(&state);
    EXPECT_NE(port.matches, nullptr);
    EXPECT_NE(port.spawn_unpublished, nullptr);
    EXPECT_NE(port.await_ready, nullptr);
    EXPECT_NE(port.rollback_actor, nullptr);
}

TEST(TopologyBootstrapTransactionTest, FakeProviderSpawnAndReady) {
    FakeProviderState state;
    auto port = make_fake_port(&state);

    config::ActorDef def;
    def.id = "test";
    def.behavior = "python:pkg:Echo";

    ConfiguredActorPlan plan{0, ConfiguredActorProviderKind::External, 7, 12345};

    auto spawn_result = port.spawn_unpublished(port.context, def, plan);
    EXPECT_TRUE(spawn_result.ok());
    EXPECT_EQ(state.spawn_count, 1u);

    auto ready_result = port.await_ready(port.context, ActorId{42}, plan,
                                          std::chrono::milliseconds(100));
    EXPECT_TRUE(ready_result.ok());
    EXPECT_EQ(state.ready_count, 1u);
}

TEST(TopologyBootstrapTransactionTest, FakeProviderRollsBackInOrder) {
    FakeProviderState state;
    state.fail_ready_at = 2; // fail on second ready

    // Simulate rollback of 3 actors.
    for (size_t i = 0; i < 3; ++i) {
        ConfiguredActorPlan plan{i, ConfiguredActorProviderKind::External, 100 + i, 0};
        (void)make_fake_port(&state).rollback_actor(&state, ActorId{}, plan);
    }
    EXPECT_EQ(state.rollback_count, 3u);
    EXPECT_EQ(state.rollback_order, (std::vector<size_t>{0, 1, 2}));
}
