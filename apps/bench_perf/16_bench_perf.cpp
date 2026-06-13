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

// =============================================================================
// HPActor App 16: Bench Perf — Actor System Performance Benchmark
// =============================================================================
//
// A performance benchmarking app that exercises two core scenarios:
//
//   1. many-actors: 5000 BenchWorkerActors with 10us CPU burn at 100Hz
//      — measures throughput under high fan-out with 8 scheduler threads.
//
//   2. hot-actor: 1 BenchHotActor (500us burn, 1000Hz) + 1000 BenchWorkerActors
//      (10us burn, 10Hz) — measures cold-actor tail latency under noisy
//      neighbor.
//
//   CLI commands (registered via static CommandRegistration<T> in
//   commands/bench_commands.cpp):
//     /bench start <preset>  — start a run
//     /bench stop            — stop the current run
//     /bench status          — show current state
//     /bench report [group]  — latency percentile + throughput report
//     /bench export          — export raw data as JSON
//     /bench list            — list available presets
//     /bench help            — show bench commands
//
// =============================================================================

#include "actors/bench_collector_actor.hpp"
#include "actors/bench_coordinator_actor.hpp"
#include "actors/bench_hot_actor.hpp"
#include "actors/bench_worker_actor.hpp"
#include "messages.hpp"

#include <hpactor/cli/cli_actor.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/core/actor_system.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

namespace bench_perf = hpactor::apps::bench_perf;

// =============================================================================
// Splash screen
// =============================================================================

static void print_splash() {
    std::cout
        << "\n"
        << "+--------------------------------------------------------------+\n"
        << "|     HPActor App 16 — Bench Perf                              |\n"
        << "|     Actor System Performance Benchmark                       |\n"
        << "+--------------------------------------------------------------+\n"
        << "|                                                              |\n"
        << "|  Presets:                                                    |\n"
        << "|    many-actors — 5000 workers, 10us burn, 100Hz              |\n"
        << "|    hot-actor   — 1 hot (500us, 1000Hz) + 1000 cold           |\n"
        << "|                                                              |\n"
        << "|  Actor pool: 5000 cold workers + 10 hot actors               |\n"
        << "|  Scheduler: 8 threads, 16384 mailbox capacity                |\n"
        << "|                                                              |\n"
        << "|  Try:                                                        |\n"
        << "|    /bench list              — see all presets                |\n"
        << "|    /bench start many-actors — run throughput test            |\n"
        << "|    /bench status            — check progress                 |\n"
        << "|    /bench report            — view results                   |\n"
        << "|    /quit                    — exit                           |\n"
        << "|                                                              |\n"
        << "+--------------------------------------------------------------+\n"
        << std::endl;
}

// =============================================================================
// Main
// =============================================================================

int main() {
    print_splash();

    // ── Configure: 8 threads, CLI enabled, large mailboxes ───────────────

    hpactor::Config config;
    config.scheduler_threads = 8;
    config.max_queue_depth = 4096;

    config.cli = hpactor::cli::CliConfig{.enabled = true,
                                         .listen_path = "",
                                         .tcp_port = 0,
                                         .default_format = "pretty",
                                         .page_size = 20,
                                         .history_path = "",
                                         .history_max = 1000};

    config.mailbox.default_capacity = 16384;

    config.shutdown_drain = hpactor::DrainConfig{
        hpactor::DrainPolicy::Drain, std::chrono::milliseconds{10'000}};

    hpactor::ActorSystem system(config);

    // ── Spawn coordinator and collector first ──────────────────────────

    auto coordinator = system.spawn<bench_perf::BenchCoordinatorActor>();
    auto collector = system.spawn<bench_perf::BenchCollectorActor>();

    // ── Determine sizes for the largest preset ─────────────────────────

    constexpr uint32_t kMaxColdWorkers = 5000;
    constexpr uint32_t kMaxHotActors = 10;

    // ── Spawn cold workers ─────────────────────────────────────────────

    std::vector<std::shared_ptr<bench_perf::BenchWorkerActor>> cold_workers;
    std::vector<hpactor::ActorAddress> cold_addrs;

    cold_workers.reserve(kMaxColdWorkers);
    cold_addrs.reserve(kMaxColdWorkers);

    for (uint32_t i = 0; i < kMaxColdWorkers; ++i) {
        auto w = system.spawn<bench_perf::BenchWorkerActor>(collector.address(), i);
        cold_addrs.push_back(w.address());
        cold_workers.push_back(std::static_pointer_cast<bench_perf::BenchWorkerActor>(
            system.get_actor(w.id())));
    }

    // ── Spawn hot actors ───────────────────────────────────────────────

    std::vector<std::shared_ptr<bench_perf::BenchHotActor>> hot_actors;
    std::vector<hpactor::ActorAddress> hot_addrs;

    hot_actors.reserve(kMaxHotActors);
    hot_addrs.reserve(kMaxHotActors);

    for (uint32_t i = 0; i < kMaxHotActors; ++i) {
        auto h = system.spawn<bench_perf::BenchHotActor>(collector.address(), i);
        hot_addrs.push_back(h.address());
        hot_actors.push_back(std::static_pointer_cast<bench_perf::BenchHotActor>(
            system.get_actor(h.id())));
    }

    // ── Wire coordinator ───────────────────────────────────────────────

    auto* coord_raw =
        static_cast<bench_perf::BenchCoordinatorActor*>(coordinator.get().get());
    coord_raw->set_worker_addrs(cold_addrs, hot_addrs, collector.address());

    // Let actors initialize before CLI takes over
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── Main loop: wait for CLI exit ───────────────────────────────────

    while (system.cli_actor() && system.cli_actor()->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::cout << "\nInitiating graceful shutdown..." << std::endl;
    auto shutdown_result = system.shutdown();
    if (shutdown_result.has_value()) {
        std::cout << "Shutdown complete." << std::endl;
    } else {
        std::cout << "Shutdown timed out — forcing exit." << std::endl;
    }

    std::cout << "=== Bench Perf Complete ===" << std::endl;
    return 0;
}
