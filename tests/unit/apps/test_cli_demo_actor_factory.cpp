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

#include <hpactor/actor/system/actor_system.hpp>

#include "apps/cli_demo/actors/aggregator_actor.hpp"
#include "apps/cli_demo/actors/broadcast_actor.hpp"
#include "apps/cli_demo/actors/clock_actor.hpp"
#include "apps/cli_demo/actors/dlq_demo_actor.hpp"
#include "apps/cli_demo/actors/health_check_actor.hpp"
#include "apps/cli_demo/actors/log_actor.hpp"
#include "apps/cli_demo/actors/query_actor.hpp"
#include "apps/cli_demo/actors/system_monitor_actor.hpp"
#include "apps/cli_demo/actors/worker_actor.hpp"
#include "apps/hpactor_demo/cli_demo_actor_factory.hpp"

#include <thread>

using namespace hpactor;
using namespace hpactor::apps::cli_demo;

TEST(CliDemoActorFactoryTest, SpawnsAllTenActors) {
    Config config;
    config.scheduler_threads = 1;
    config.cli.enabled = false;
    ActorSystem system(config);

    auto actors = spawn_cli_demo_actors(system);

    EXPECT_NE(actors.aggregator, nullptr);
    EXPECT_NE(actors.health_check, nullptr);
    EXPECT_NE(actors.broadcast, nullptr);
    EXPECT_NE(actors.clock, nullptr);
    EXPECT_NE(actors.log, nullptr);
    EXPECT_NE(actors.monitor, nullptr);
    EXPECT_NE(actors.dlq_demo, nullptr);
    EXPECT_NE(actors.query, nullptr);
    for (int i = 0; i < 4; ++i) {
        EXPECT_NE(actors.workers[i], nullptr) << "Worker " << (i + 1) << " is null";
    }
}

TEST(CliDemoActorFactoryTest, WorkerConfigsAreCorrect) {
    Config config;
    config.scheduler_threads = 1;
    config.cli.enabled = false;
    ActorSystem system(config);

    auto actors = spawn_cli_demo_actors(system);

    // Worker-1: rate limiter at 100 msg/s
    EXPECT_GT(actors.workers[0]->rate_limit(), 0.0);
    EXPECT_NEAR(actors.workers[0]->rate_limit(), 100.0, 1.0);
    EXPECT_EQ(actors.workers[0]->rate_burst(), 10u);

    // Worker-2: rate limiter at 500 msg/s
    EXPECT_NEAR(actors.workers[1]->rate_limit(), 500.0, 1.0);
    EXPECT_EQ(actors.workers[1]->rate_burst(), 50u);

    // Worker-3: no rate limit (circuit breaker mode)
    EXPECT_DOUBLE_EQ(actors.workers[2]->rate_limit(), 0.0);

    // Worker-4: no rate limit (delivery failure mode)
    EXPECT_DOUBLE_EQ(actors.workers[3]->rate_limit(), 0.0);
}

TEST(CliDemoActorFactoryTest, KickoffDoesNotCrash) {
    Config config;
    config.scheduler_threads = 1;
    config.cli.enabled = false;
    ActorSystem system(config);

    auto actors = spawn_cli_demo_actors(system);

    kickoff_cli_demo_actors(system, actors);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    SUCCEED();
}
