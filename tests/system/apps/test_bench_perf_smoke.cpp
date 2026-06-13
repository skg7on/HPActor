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

// Smoke test: spawn a minimal bench_perf setup, run a short benchmark, and
// verify the collector receives latency samples from both cold workers and
// the hot actor.

#include <gtest/gtest.h>

#include <hpactor/core/actor_system.hpp>

#include <apps/bench_perf/actors/bench_collector_actor.hpp>
#include <apps/bench_perf/actors/bench_coordinator_actor.hpp>
#include <apps/bench_perf/actors/bench_hot_actor.hpp>
#include <apps/bench_perf/actors/bench_worker_actor.hpp>
#include <apps/bench_perf/messages.hpp>

#include <system_test_fixture.hpp>

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bench_perf = hpactor::apps::bench_perf;
using namespace hpactor;

TEST(BenchPerfSmoke, SpawnAndRunMinimal) {
    // Build config: 2 scheduler threads, no CLI/network/tracing
    Config cfg = test::config_with_scheduler(2);
    cfg.max_queue_depth = 256;
    cfg.mailbox.default_capacity = 1024;

    ActorSystem system(cfg);

    // Spawn collector
    auto collector = system.spawn<bench_perf::BenchCollectorActor>();

    // Spawn 5 cold workers
    std::vector<ActorAddress> cold_addrs;
    for (uint32_t i = 0; i < 5; ++i) {
        auto w = system.spawn<bench_perf::BenchWorkerActor>(collector.address(), i);
        cold_addrs.push_back(w.address());
    }

    // Spawn 1 hot actor
    auto hot = system.spawn<bench_perf::BenchHotActor>(collector.address(), 0);

    // Spawn coordinator
    auto coordinator = system.spawn<bench_perf::BenchCoordinatorActor>();

    // Wire coordinator with worker/hot actor addresses
    auto* coord_raw = std::static_pointer_cast<bench_perf::BenchCoordinatorActor>(
                          system.get_actor(coordinator.id()))
                          .get();
    coord_raw->set_worker_addrs(cold_addrs, {hot.address()}, collector.address());

    // Let actors initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Start a run with the "hot-actor" preset
    //  hot-actor: 1 hot (500us, 1000Hz) + 1000 cold workers (10us, 10Hz) in
    //  the preset, but we spawned only 5 cold workers + 1 hot actor
    {
        std::string preset = "hot-actor";
        StreamBuffer payload(preset.begin(), preset.end());
        system.deliver_local(coordinator.id(),
                             bench_perf::make_msg(bench_perf::BenchStartTag,
                                                  std::move(payload)));
    }

    // Poll for samples with generous timeout
    auto* coll_raw = std::static_pointer_cast<bench_perf::BenchCollectorActor>(
                         system.get_actor(collector.id()))
                         .get();

    // Each cold worker at 10Hz sends ~5 ticks in 500ms; 5 workers => ~25
    // hot actor at 1000Hz sends ~500 ticks in 500ms
    bool got_samples = test::assert_eventually(
        [coll_raw]() {
            return coll_raw->total_cold_samples() > 0 &&
                   coll_raw->total_hot_samples() > 0;
        },
        /*deadline_ms=*/10000);

    // Stop the run
    system.deliver_local(coordinator.id(),
                         bench_perf::make_msg(bench_perf::BenchStopTag));

    // Let stop propagate and collector recompute percentiles
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Verify collector received samples
    uint64_t cold_samples = coll_raw->total_cold_samples();
    uint64_t hot_samples = coll_raw->total_hot_samples();

    EXPECT_TRUE(got_samples)
        << "Timed out waiting for samples. cold=" << cold_samples
        << " hot=" << hot_samples;

    EXPECT_GT(cold_samples, 0u)
        << "Expected at least some cold worker samples but got " << cold_samples;

    EXPECT_GT(hot_samples, 0u)
        << "Expected at least some hot actor samples but got " << hot_samples;

    // Clean shutdown
    auto shutdown_result = system.shutdown();
    EXPECT_TRUE(shutdown_result.has_value())
        << "Shutdown failed: " << shutdown_result.error().message();
}
