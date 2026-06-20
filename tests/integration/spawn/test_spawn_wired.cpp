// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/spawn.hpp>
#include <hpactor/msg/request_timeout.hpp>

namespace hpactor {
namespace {

TEST(SpawnWiredTest, NetworkDisabledReturnsErrorImmediately) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    StreamBuffer args;
    auto handle = system.spawn_remote_async("node1", "calculator", args);

    EXPECT_TRUE(handle.ready());
    auto r = handle.get();
    EXPECT_FALSE(r.has_value());
}

TEST(SpawnWiredTest, SpawnTimeoutOverrideAccepted) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    StreamBuffer args;
    auto handle = system.spawn_remote_async("node1", "calculator", args,
                                            RequestTimeout::from_ms(10000));

    EXPECT_TRUE(handle.ready()); // immediately resolved with network disabled
}

TEST(SpawnWiredTest, AsyncActorAliasWorks) {
    // Verify backward compat alias works with default construction
    AsyncActor handle;
    EXPECT_FALSE(handle.ready());
}

TEST(SpawnWiredTest, SpawnRemoteSyncUsesRequestTimeout) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    StreamBuffer args;
    auto r = system.spawn_remote("node1", "calculator", args,
                                 RequestTimeout::from_ms(5000));
    EXPECT_FALSE(r.has_value());
}

TEST(SpawnWiredTest, SpawnRemoteSyncDefaultTimeout) {
    Config cfg;
    cfg.scheduler_threads = 0;
    cfg.enable_network = false;
    ActorSystem system(cfg);

    StreamBuffer args;
    // Uses default RequestTimeout (system spawn_timeout_ms)
    auto r = system.spawn_remote("node1", "calculator", args);
    EXPECT_FALSE(r.has_value());
}

} // namespace
} // namespace hpactor
