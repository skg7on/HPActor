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

#pragma once

#include "actors/actor_creation_actor.hpp"
#include "caf_bench_config.hpp"
#include "caf_bench_metrics.hpp"
#include "caf_bench_sampler.hpp"

#include <hpactor/actor/actor_system.hpp>

#include <chrono>
#include <thread>

namespace hpactor::apps::bench_caf {

inline Config make_bench_actor_config(const CafBenchConfig& cfg) {
    Config runtime;
    runtime.scheduler_threads = static_cast<size_t>(cfg.scheduler_threads);
    runtime.max_queue_depth = static_cast<size_t>(cfg.mailbox_capacity);
    runtime.mailbox.default_capacity = cfg.mailbox_capacity;
    runtime.enable_network = false;
    runtime.enable_receptionist = false;
    runtime.cli.enabled = false;
    runtime.tracing.enabled = false;
    return runtime;
}

inline TrialMetrics
run_actor_creation_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    ActorCreationCounters counters;
    uint32_t depth = actor_creation_depth_for_preset(cfg.preset);
    uint64_t expected = actor_creation_expected_count(depth);

    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    ActorSystem system(make_bench_actor_config(cfg));
    auto root = system.spawn<ActorCreationNodeActor>(&counters, depth);
    system.deliver_local(root.id(), make_bench_msg(ActorCreationStartTag));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counters.completed.load(std::memory_order_acquire) < expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    metrics.actors_created = counters.created.load();
    metrics.actors_completed = counters.completed.load();
    metrics.completed = shutdown.has_value() &&
                        metrics.actors_created == expected &&
                        metrics.actors_completed == expected;
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

} // namespace hpactor::apps::bench_caf
