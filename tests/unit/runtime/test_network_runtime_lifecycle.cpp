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

#include "src/runtime/network_runtime.hpp"

#include <hpactor/types/types.hpp>

#include <gtest/gtest.h>

#include <thread>

namespace hpactor {
namespace {

// ── Fixture ──────────────────────────────────────────────────────────────────

class NetworkRuntimeLifecycleTest : public ::testing::Test {
  protected:
    // Default disabled config for negative tests.
    NetworkRuntimeConfig disabled_config;

    // Enabled config for positive lifecycle tests.
    NetworkRuntimeConfig enabled_config;

    // Null dependencies — sufficient for state-machine-only tests.
    NetworkRuntime::Dependencies null_deps;

    void SetUp() override {
        disabled_config.enabled = false;

        enabled_config.enabled = true;
        enabled_config.tcp_port = 0; // Don't actually bind.
    }
};

// ── Construction & Disabled State ────────────────────────────────────────────

TEST_F(NetworkRuntimeLifecycleTest, ConstructedStateIsConstructed) {
    NetworkRuntime runtime(disabled_config, null_deps);
    EXPECT_EQ(runtime.state(), NetworkRuntime::State::Constructed);
}

TEST_F(NetworkRuntimeLifecycleTest, StartReturnsErrorWhenDisabled) {
    NetworkRuntime runtime(disabled_config, null_deps);
    auto result = runtime.start();
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error().code(), errors::network_disabled);
}

TEST_F(NetworkRuntimeLifecycleTest, AccessorsReturnNullWhenDisabled) {
    NetworkRuntime runtime(disabled_config, null_deps);
    EXPECT_EQ(runtime.event_loop(), nullptr);
    EXPECT_EQ(runtime.transport(), nullptr);
    EXPECT_EQ(runtime.discovery(), nullptr);
    EXPECT_EQ(runtime.registrar(), nullptr);
    EXPECT_EQ(runtime.rpc_channel(), nullptr);
    EXPECT_EQ(runtime.http_client(), nullptr);
    EXPECT_EQ(runtime.location_cache(), nullptr);
}

TEST_F(NetworkRuntimeLifecycleTest, SnapshotWhenDisabled) {
    NetworkRuntime runtime(disabled_config, null_deps);
    auto snap = runtime.snapshot();
    EXPECT_EQ(snap.enabled, false);
    EXPECT_EQ(snap.state, static_cast<uint8_t>(NetworkRuntime::State::Constructed));
}

// ── Lifecycle Transitions ────────────────────────────────────────────────────

TEST_F(NetworkRuntimeLifecycleTest, StartTransitionsToRunning) {
    NetworkRuntime runtime(enabled_config, null_deps);
    auto result = runtime.start();
    // start() is a skeleton — it succeeds without real network work.
    ASSERT_FALSE(result.is_error()) << result.error().message();
    EXPECT_EQ(runtime.state(), NetworkRuntime::State::Running);
}

TEST_F(NetworkRuntimeLifecycleTest, StartIsIdempotent) {
    NetworkRuntime runtime(enabled_config, null_deps);
    auto r1 = runtime.start();
    ASSERT_FALSE(r1.is_error());
    // Second start while already running should succeed (idempotent).
    auto r2 = runtime.start();
    EXPECT_FALSE(r2.is_error());
    EXPECT_EQ(runtime.state(), NetworkRuntime::State::Running);
}

TEST_F(NetworkRuntimeLifecycleTest, StopTransitionsToStopped) {
    NetworkRuntime runtime(enabled_config, null_deps);
    auto r1 = runtime.start();
    ASSERT_FALSE(r1.is_error());

    auto r2 = runtime.stop();
    ASSERT_FALSE(r2.is_error()) << r2.error().message();
    EXPECT_EQ(runtime.state(), NetworkRuntime::State::Stopped);
}

TEST_F(NetworkRuntimeLifecycleTest, StopIsIdempotent) {
    NetworkRuntime runtime(enabled_config, null_deps);
    runtime.start();
    runtime.stop();
    // Second stop should succeed (idempotent).
    auto r3 = runtime.stop();
    EXPECT_FALSE(r3.is_error());
    EXPECT_EQ(runtime.state(), NetworkRuntime::State::Stopped);
}

TEST_F(NetworkRuntimeLifecycleTest, StartAfterStopReturnsError) {
    NetworkRuntime runtime(enabled_config, null_deps);
    runtime.start();
    runtime.stop();

    auto result = runtime.start();
    EXPECT_TRUE(result.is_error());
    EXPECT_EQ(result.error().code(), errors::network_already_stopped);
}

TEST_F(NetworkRuntimeLifecycleTest, SnapshotReflectsRunningState) {
    NetworkRuntime runtime(enabled_config, null_deps);
    runtime.start();
    auto snap = runtime.snapshot();
    EXPECT_EQ(snap.enabled, true);
    EXPECT_EQ(snap.state, static_cast<uint8_t>(NetworkRuntime::State::Running));
}

TEST_F(NetworkRuntimeLifecycleTest, SnapshotReflectsStoppedState) {
    NetworkRuntime runtime(enabled_config, null_deps);
    runtime.start();
    runtime.stop();
    auto snap = runtime.snapshot();
    EXPECT_EQ(snap.state, static_cast<uint8_t>(NetworkRuntime::State::Stopped));
}

// ── Destructor Safety ────────────────────────────────────────────────────────

TEST_F(NetworkRuntimeLifecycleTest, DestructorStopsWithoutExplicitStop) {
    // Construct, start, let destructor call stop().
    {
        NetworkRuntime runtime(enabled_config, null_deps);
        runtime.start();
        // No explicit stop() — destructor handles it.
    }
    SUCCEED();
}

TEST_F(NetworkRuntimeLifecycleTest, DestructorIsSafeWhenNeverStarted) {
    {
        NetworkRuntime runtime(enabled_config, null_deps);
        // Never start, no explicit stop — destructor handles it.
    }
    SUCCEED();
}

// ── Concurrent State Access ──────────────────────────────────────────────────

TEST_F(NetworkRuntimeLifecycleTest, StateIsReadableFromMultipleThreads) {
    NetworkRuntime runtime(enabled_config, null_deps);
    runtime.start();

    std::atomic<bool> done{false};
    std::thread reader([&]() {
        while (!done.load(std::memory_order_acquire)) {
            auto s = runtime.state();
            // State must be either Running or Stopping/Stopped (never
            // garbage).
            EXPECT_TRUE(s == NetworkRuntime::State::Running ||
                        s == NetworkRuntime::State::Stopping ||
                        s == NetworkRuntime::State::Stopped)
                << "Unexpected state: " << static_cast<int>(s);
        }
    });

    // Let reader run briefly.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    runtime.stop();
    done.store(true, std::memory_order_release);
    reader.join();

    EXPECT_EQ(runtime.state(), NetworkRuntime::State::Stopped);
}

} // namespace
} // namespace hpactor
