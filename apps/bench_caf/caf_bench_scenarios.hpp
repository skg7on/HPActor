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
#include "actors/mailbox_n1_actor.hpp"
#include "actors/mixed_case_actor.hpp"
#include "actors/traffic_distribution_actor.hpp"
#include "caf_bench_config.hpp"
#include "caf_bench_metrics.hpp"
#include "caf_bench_sampler.hpp"

#include <hpactor/actor/actor_system.hpp>

#include <algorithm>
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

inline TrialMetrics
run_mailbox_n1_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    auto dims = mailbox_n1_dimensions_for_preset(cfg.preset);
    uint64_t expected =
        static_cast<uint64_t>(dims.senders) * dims.messages_per_sender;

    // Size the mailbox to hold all expected messages so the bounded
    // admission policy does not reject before the receiver drains them.
    auto system_cfg = make_bench_actor_config(cfg);
    system_cfg.mailbox.default_capacity =
        static_cast<uint32_t>(std::max<uint64_t>(expected, 4096));

    MailboxN1Counters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    ActorSystem system(system_cfg);
    auto receiver = system.spawn<MailboxN1ReceiverActor>(&counters);

    for (uint32_t i = 0; i < dims.senders; ++i) {
        auto sender = system.spawn<MailboxN1SenderActor>(
            &counters, receiver.address(), i, dims.messages_per_sender,
            cfg.message_size_bytes, cfg.seed + i);
        system.deliver_local(sender.id(), make_bench_msg(MailboxLoadTag));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counters.received.load(std::memory_order_acquire) < expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    metrics.total_sent = counters.sent.load();
    metrics.total_received = counters.received.load();
    metrics.total_dropped = 0;
    metrics.completed = shutdown.has_value() && metrics.total_sent == expected &&
                        metrics.total_received == expected;
    metrics.throughput_msgps =
        throughput(metrics.total_received, metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

inline TrialMetrics
run_mixed_case_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    auto dims = mixed_case_dimensions_for_preset(cfg.preset);
    uint64_t expected_rings = static_cast<uint64_t>(dims.rings) * dims.repetitions;
    uint64_t expected_cpu = dims.rings;
    uint64_t expected_hops =
        static_cast<uint64_t>(dims.rings) * dims.repetitions * dims.token_value;

    MixedCaseCounters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    ActorSystem system(make_bench_actor_config(cfg));

    for (uint32_t r = 0; r < dims.rings; ++r) {
        std::vector<ActorAddress> nodes;
        nodes.reserve(dims.ring_size);
        for (uint32_t i = 0; i < dims.ring_size; ++i) {
            auto node =
                system.spawn<MixedRingNodeActor>(&counters, ActorAddress{});
            nodes.push_back(node.address());
        }

        for (uint32_t i = 0; i < dims.ring_size; ++i) {
            auto actor = std::static_pointer_cast<MixedRingNodeActor>(
                system.get_actor(nodes[i].id));
            actor->set_next(nodes[(i + 1) % dims.ring_size]);
        }

        auto cpu = system.spawn<MixedCpuActor>(&counters);
        system.deliver_local(cpu.id(), make_bench_msg(MixedCpuTaskTag));

        for (uint32_t rep = 0; rep < dims.repetitions; ++rep) {
            BenchPayloadHeader header;
            header.sequence = dims.token_value - 1;
            system.deliver_local(
                nodes[0].id, make_bench_msg(MixedTokenTag,
                                            encode_bench_payload(header, 0, rep)));
        }
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while ((counters.rings_completed.load(std::memory_order_acquire) < expected_rings ||
            counters.cpu_tasks_completed.load(std::memory_order_acquire) <
                expected_cpu) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    metrics.actors_created = counters.actors_created.load();
    metrics.rings_completed = counters.rings_completed.load();
    metrics.token_hops = counters.token_hops.load();
    metrics.cpu_tasks_completed = counters.cpu_tasks_completed.load();
    metrics.completed = shutdown.has_value() &&
                        metrics.rings_completed == expected_rings &&
                        metrics.cpu_tasks_completed == expected_cpu &&
                        metrics.token_hops == expected_hops;
    metrics.throughput_msgps = throughput(metrics.token_hops, metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

// ── Phase 2: Traffic Distribution Scenarios ────────────────────

inline OneToNDimensions one_to_n_dimensions_for_preset(PresetKind preset) {
    switch (preset) {
        case PresetKind::Smoke:
            return {8, 1000};
        case PresetKind::Nightly:
            return {16, 5000};
        case PresetKind::PaperScale:
            return {32, 10000};
        case PresetKind::Stress:
            return {64, 10000};
    }
    return {8, 1000};
}

inline NToNRandomDimensions n_to_n_random_dimensions_for_preset(PresetKind preset) {
    switch (preset) {
        case PresetKind::Smoke:
            return {4, 4, 250};
        case PresetKind::Nightly:
            return {8, 8, 1000};
        case PresetKind::PaperScale:
            return {16, 16, 5000};
        case PresetKind::Stress:
            return {32, 32, 10000};
    }
    return {4, 4, 250};
}

inline TrialMetrics
run_one_to_one_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    uint32_t messages = 10000;
    DistributionCounters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    auto system_cfg = make_bench_actor_config(cfg);
    system_cfg.mailbox.default_capacity =
        static_cast<uint32_t>(std::max<uint64_t>(messages, 4096));
    ActorSystem system(system_cfg);

    auto receiver = system.spawn<OneToOneReceiver>(&counters);
    auto sender =
        system.spawn<OneToOneSender>(&counters, receiver.address(), messages,
                                     cfg.message_size_bytes, cfg.seed);
    system.deliver_local(sender.id(), make_bench_msg(MailboxLoadTag));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counters.received.load(std::memory_order_acquire) < messages &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    metrics.total_sent = counters.sent.load();
    metrics.total_received = counters.received.load();
    metrics.completed = shutdown.has_value() && metrics.total_sent == messages &&
                        metrics.total_received == messages;
    metrics.throughput_msgps =
        throughput(metrics.total_received, metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

inline TrialMetrics
run_one_to_n_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    auto dims = one_to_n_dimensions_for_preset(cfg.preset);
    uint64_t expected =
        static_cast<uint64_t>(dims.receivers) * dims.messages_per_receiver;

    DistributionCounters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    auto system_cfg = make_bench_actor_config(cfg);
    system_cfg.mailbox.default_capacity =
        static_cast<uint32_t>(std::max<uint64_t>(expected, 4096));
    ActorSystem system(system_cfg);

    std::vector<ActorAddress> receivers;
    receivers.reserve(dims.receivers);
    for (uint32_t i = 0; i < dims.receivers; ++i) {
        auto r = system.spawn<OneToNReceiver>(&counters);
        receivers.push_back(r.address());
    }
    auto sender = system.spawn<OneToNSender>(&counters, receivers,
                                             dims.messages_per_receiver,
                                             cfg.message_size_bytes, cfg.seed);
    system.deliver_local(sender.id(), make_bench_msg(MailboxLoadTag));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counters.received.load(std::memory_order_acquire) < expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    metrics.total_sent = counters.sent.load();
    metrics.total_received = counters.received.load();
    metrics.completed = shutdown.has_value() && metrics.total_sent == expected &&
                        metrics.total_received == expected;
    metrics.throughput_msgps =
        throughput(metrics.total_received, metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

inline TrialMetrics
run_n_to_n_random_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    auto dims = n_to_n_random_dimensions_for_preset(cfg.preset);
    uint64_t expected =
        static_cast<uint64_t>(dims.senders) * dims.messages_per_sender;

    DistributionCounters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    auto system_cfg = make_bench_actor_config(cfg);
    system_cfg.mailbox.default_capacity =
        static_cast<uint32_t>(std::max<uint64_t>(expected, 4096));
    ActorSystem system(system_cfg);

    std::vector<ActorAddress> receivers;
    receivers.reserve(dims.receivers);
    for (uint32_t i = 0; i < dims.receivers; ++i) {
        auto r = system.spawn<OneToNReceiver>(&counters);
        receivers.push_back(r.address());
    }
    for (uint32_t i = 0; i < dims.senders; ++i) {
        auto s = system.spawn<NToNRandomSender>(
            &counters, receivers, i, dims.messages_per_sender,
            cfg.message_size_bytes, cfg.seed + i);
        system.deliver_local(s.id(), make_bench_msg(MailboxLoadTag));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counters.received.load(std::memory_order_acquire) < expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    metrics.total_sent = counters.sent.load();
    metrics.total_received = counters.received.load();
    metrics.completed = shutdown.has_value() && metrics.total_sent == expected &&
                        metrics.total_received == expected;
    metrics.throughput_msgps =
        throughput(metrics.total_received, metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

} // namespace hpactor::apps::bench_caf
