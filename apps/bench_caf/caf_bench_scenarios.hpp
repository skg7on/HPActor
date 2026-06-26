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
#include "actors/distributed_ping_actor.hpp"
#include "actors/mailbox_n1_actor.hpp"
#include "actors/mandelbrot_actor.hpp"
#include "actors/mixed_case_actor.hpp"
#include "actors/scheduling_mix_actor.hpp"
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

// ── Ring traffic: M actors in a ring, token loops K times ─────

inline TrialMetrics
run_ring_traffic_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    constexpr uint32_t kNodes = 16;
    constexpr uint32_t kLaps = 100;
    constexpr uint64_t kExpectedHops = kNodes * kLaps;

    DistributionCounters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    ActorSystem system(make_bench_actor_config(cfg));

    // Spawn ring nodes; each sends to the next.
    // Use RingNode inline (derived from EventBasedActor pattern).
    struct RingNode : public EventBasedActor {
        DistributionCounters* c;
        ActorAddress next;
        uint64_t seed_ = 0;
        RingNode(ActorContext* ctx, ActorSystem& sys, DistributionCounters* cnt,
                 ActorAddress n)
            : EventBasedActor(ctx, sys), c(cnt), next(n) {
            become(make_behavior());
        }
        void set_next(ActorAddress n) {
            next = n;
        }
        Behavior make_behavior() override {
            return Behavior{[this](TypedMessage& msg) {
                if (msg.type_id() != MailboxLoadTag)
                    return;
                c->token_hops.fetch_add(1, std::memory_order_relaxed);
                auto remaining = decode_bench_payload(msg.payload()).sequence;
                if (remaining == 0) {
                    c->receivers_done.fetch_add(1, std::memory_order_release);
                    return;
                }
                BenchPayloadHeader h;
                h.sequence = remaining - 1;
                context()->send(
                    next, make_bench_msg(MailboxLoadTag,
                                         encode_bench_payload(h, 0, seed_)));
            }};
        }
    };

    std::vector<ActorAddress> nodes(kNodes);
    for (uint32_t i = 0; i < kNodes; ++i) {
        auto n = system.spawn<RingNode>(&counters, ActorAddress{});
        nodes[i] = n.address();
    }
    for (uint32_t i = 0; i < kNodes; ++i) {
        auto actor =
            std::static_pointer_cast<RingNode>(system.get_actor(nodes[i].id));
        actor->set_next(nodes[(i + 1) % kNodes]);
        actor->seed_ = cfg.seed + i;
    }

    // Start token at first node.  Token will loop kLaps * kNodes hop distance.
    BenchPayloadHeader init;
    init.sequence = kLaps * kNodes - 1;
    system.deliver_local(
        nodes[0].id, make_bench_msg(MailboxLoadTag,
                                    encode_bench_payload(init, 0, uint64_t{0})));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counters.receivers_done.load(std::memory_order_acquire) < 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    metrics.token_hops = counters.token_hops.load();
    metrics.total_sent = kExpectedHops;
    metrics.total_received = kExpectedHops;
    metrics.completed = shutdown.has_value() && metrics.token_hops >= kExpectedHops;
    metrics.throughput_msgps = throughput(metrics.token_hops, metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

// ── Pipeline: M stages, stage 0 generates, each forwards ───────

inline TrialMetrics
run_pipeline_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    constexpr uint32_t kStages = 8;
    constexpr uint32_t kMessages = 100;

    DistributionCounters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    ActorSystem system(make_bench_actor_config(cfg));

    struct PipelineStage : public EventBasedActor {
        DistributionCounters* c;
        ActorAddress next;
        PipelineStage(ActorContext* ctx, ActorSystem& sys,
                      DistributionCounters* cnt, ActorAddress n)
            : EventBasedActor(ctx, sys), c(cnt), next(n) {
            become(make_behavior());
        }
        Behavior make_behavior() override {
            return Behavior{[this](TypedMessage& msg) {
                if (msg.type_id() != MailboxLoadTag)
                    return;
                c->received.fetch_add(1, std::memory_order_relaxed);
                auto hdr = decode_bench_payload(msg.payload());
                if (hdr.sender_id == 0xFF) {
                    c->receivers_done.fetch_add(1, std::memory_order_release);
                    return;
                }
                if (next != ActorAddress{}) {
                    context()->send(
                        next, make_bench_msg(MailboxLoadTag,
                                             encode_bench_payload(hdr, 0, 0)));
                } else {
                    c->receivers_done.fetch_add(1, std::memory_order_release);
                }
            }};
        }
    };

    std::vector<ActorAddress> stages(kStages);
    for (uint32_t i = 0; i < kStages; ++i) {
        auto s = system.spawn<PipelineStage>(&counters, ActorAddress{});
        stages[i] = s.address();
    }
    for (uint32_t i = 0; i < kStages - 1; ++i) {
        auto actor = std::static_pointer_cast<PipelineStage>(
            system.get_actor(stages[i].id));
        actor->next = stages[i + 1];
    }

    // Inject messages at stage 0.
    for (uint32_t i = 0; i < kMessages; ++i) {
        BenchPayloadHeader h;
        h.sender_id = i + 1;
        h.sequence = i;
        system.deliver_local(
            stages[0].id, make_bench_msg(MailboxLoadTag,
                                         encode_bench_payload(h, 0, uint64_t{0})));
        counters.sent.fetch_add(1, std::memory_order_relaxed);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counters.receivers_done.load(std::memory_order_acquire) < kMessages &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    metrics.total_sent = counters.sent.load();
    metrics.total_received = counters.receivers_done.load();
    metrics.completed = shutdown.has_value() && metrics.total_sent == kMessages &&
                        counters.receivers_done.load() >= kMessages;
    metrics.throughput_msgps =
        throughput(metrics.total_received, metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

// ── Zipf hotspot: skewed receiver selection ─────────────────────

inline TrialMetrics
run_zipf_hotspot_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    constexpr uint32_t kSenders = 4;
    constexpr uint32_t kReceivers = 4;
    constexpr uint32_t kMessagesPerSender = 5000;
    uint64_t expected = kSenders * kMessagesPerSender;

    DistributionCounters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    auto system_cfg = make_bench_actor_config(cfg);
    system_cfg.mailbox.default_capacity =
        static_cast<uint32_t>(std::max<uint64_t>(expected, 4096));
    ActorSystem system(system_cfg);

    std::vector<ActorAddress> receivers;
    receivers.reserve(kReceivers);
    for (uint32_t i = 0; i < kReceivers; ++i) {
        auto r = system.spawn<OneToNReceiver>(&counters);
        receivers.push_back(r.address());
    }

    // Zipf-distributed senders: each sender uses a weighted LCG to select
    // receivers with Zipf-like skew (rank 0 gets ~50%, rank 1 ~25%, etc.).
    for (uint32_t s = 0; s < kSenders; ++s) {
        auto sender = system.spawn<NToNRandomSender>(
            &counters, receivers, s, kMessagesPerSender, cfg.message_size_bytes,
            cfg.seed + s);
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
    metrics.completed = shutdown.has_value() && metrics.total_sent == expected &&
                        metrics.total_received == expected;
    metrics.throughput_msgps =
        throughput(metrics.total_received, metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

// ── Bursty waves: send in batches with deterministic pacing ──────

inline TrialMetrics
run_bursty_waves_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    constexpr uint32_t kWaves = 5;
    constexpr uint32_t kMessagesPerWave = 4000;
    constexpr uint32_t kSenders = 4;
    uint64_t expected = kWaves * kMessagesPerWave;

    DistributionCounters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    auto system_cfg = make_bench_actor_config(cfg);
    system_cfg.mailbox.default_capacity =
        static_cast<uint32_t>(std::max<uint64_t>(expected, 4096));
    ActorSystem system(system_cfg);

    auto receiver = system.spawn<OneToOneReceiver>(&counters);
    for (uint32_t s = 0; s < kSenders; ++s) {
        auto sender = system.spawn<OneToOneSender>(
            &counters, receiver.address(), kMessagesPerWave / kSenders,
            cfg.message_size_bytes, cfg.seed + s);
        system.deliver_local(sender.id(), make_bench_msg(MailboxLoadTag));
    }

    // The senders all run at once (wave 1). For a real multi-wave benchmark,
    // we'd need timer-based pacing. For smoke, single-wave burst suffices
    // to validate the topology. The counters track sent/received.

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counters.received.load(std::memory_order_acquire) < kMessagesPerWave &&
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
    metrics.completed = shutdown.has_value() &&
                        metrics.total_sent >= kMessagesPerWave &&
                        metrics.total_received >= kMessagesPerWave;
    metrics.throughput_msgps =
        throughput(metrics.total_received, metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

// ── Mandelbrot CPU scheduling ─────────────────────────────────

inline TrialMetrics
run_mandelbrot_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    auto dims = mandel_dimensions_for_preset(cfg.preset);
    MandelCounters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    ActorSystem system(make_bench_actor_config(cfg));

    double xmin = -2.0, xmax = 1.0, ymin = -1.5, ymax = 1.5;
    uint32_t rows_per_worker = dims.height / dims.workers;
    metrics.actors_created = dims.workers;

    for (uint32_t w = 0; w < dims.workers; ++w) {
        uint32_t row_start = w * rows_per_worker;
        uint32_t row_end =
            (w == dims.workers - 1) ? dims.height : (w + 1) * rows_per_worker;
        auto worker = system.spawn<MandelWorkerActor>(
            &counters, xmin, xmax, ymin, ymax, dims.width, row_start, row_end,
            dims.max_iterations);
        system.deliver_local(worker.id(), make_bench_msg(MandelTaskTag));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counters.workers_done.load(std::memory_order_acquire) < dims.workers &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    metrics.cpu_tasks_completed = counters.workers_done.load();
    metrics.completed =
        shutdown.has_value() && metrics.cpu_tasks_completed >= dims.workers;
    metrics.throughput_msgps = throughput(
        static_cast<uint64_t>(dims.width) * dims.height, metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

// ── Scheduling-mix: concurrent spawn + CPU + ring workloads ────

inline TrialMetrics
run_scheduling_mix_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    auto dims = sched_mix_dimensions_for_preset(cfg.preset);
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    ActorSystem system(make_bench_actor_config(cfg));

    // Separate counters per workload type for type safety.
    ActorCreationCounters ac_counters;
    MixedCaseCounters mc_counters;

    // Wave 1: spawn creation trees.
    for (uint32_t w = 0; w < dims.waves; ++w) {
        auto root =
            system.spawn<ActorCreationNodeActor>(&ac_counters, dims.tree_depth);
        system.deliver_local(root.id(), make_bench_msg(ActorCreationStartTag));
    }

    // Wave 2: fire CPU tasks.
    for (uint32_t i = 0; i < dims.pool_size; ++i) {
        auto cpu = system.spawn<MixedCpuActor>(&mc_counters);
        system.deliver_local(cpu.id(), make_bench_msg(MixedCpuTaskTag));
    }

    // Wave 3: start a token ring.
    std::vector<ActorAddress> nodes(dims.ring_size);
    for (uint32_t i = 0; i < dims.ring_size; ++i) {
        auto n = system.spawn<MixedRingNodeActor>(&mc_counters, ActorAddress{});
        nodes[i] = n.address();
    }
    for (uint32_t i = 0; i < dims.ring_size; ++i) {
        auto actor = std::static_pointer_cast<MixedRingNodeActor>(
            system.get_actor(nodes[i].id));
        actor->set_next(nodes[(i + 1) % dims.ring_size]);
    }
    BenchPayloadHeader init;
    init.sequence = dims.ring_messages - 1;
    system.deliver_local(
        nodes[0].id,
        make_bench_msg(MixedTokenTag, encode_bench_payload(init, 0, uint64_t{0})));

    uint64_t expected_actors =
        actor_creation_expected_count(dims.tree_depth) * dims.waves;
    uint64_t expected_cpu = dims.pool_size;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while ((ac_counters.created.load(std::memory_order_acquire) < expected_actors ||
            mc_counters.cpu_tasks_completed.load(std::memory_order_acquire) <
                expected_cpu) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    metrics.actors_created = ac_counters.created.load();
    metrics.cpu_tasks_completed = mc_counters.cpu_tasks_completed.load();
    metrics.token_hops = mc_counters.token_hops.load();
    metrics.completed = shutdown.has_value() &&
                        metrics.actors_created >= expected_actors &&
                        metrics.cpu_tasks_completed >= expected_cpu;
    metrics.throughput_msgps = throughput(metrics.token_hops, metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

// ── Distributed ping/pong (loopback smoke) ─────────────────────

inline TrialMetrics
run_distributed_ping_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    constexpr uint32_t kActorsPerNode = 2;
    constexpr uint32_t kPingsPerTarget = 100;
    DistributedPingCounters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    auto system_cfg = make_bench_actor_config(cfg);
    system_cfg.mailbox.default_capacity = 4096;
    ActorSystem system(system_cfg);

    // Two logical "nodes" of actors.  Group A actors ping Group B actors.
    std::vector<ActorAddress> group_a, group_b;
    for (uint32_t i = 0; i < kActorsPerNode; ++i) {
        auto a = system.spawn<PingActor>(&counters, std::vector<ActorAddress>{},
                                         0, cfg.seed + i);
        auto b = system.spawn<PingActor>(&counters, std::vector<ActorAddress>{},
                                         0, cfg.seed + 100 + i);
        group_a.push_back(a.address());
        group_b.push_back(b.address());
    }

    // Configure cross-targets: each A actor pings all B actors.
    for (auto& addr : group_a) {
        auto actor =
            std::static_pointer_cast<PingActor>(system.get_actor(addr.id));
        actor->set_targets(group_b);
        actor->set_pings_per_target(kPingsPerTarget);
    }
    for (auto& addr : group_b) {
        auto actor =
            std::static_pointer_cast<PingActor>(system.get_actor(addr.id));
        actor->set_targets(group_a);
        actor->set_pings_per_target(kPingsPerTarget);
    }

    // Kick off ping/pong from group A.
    for (const auto& addr : group_a) {
        system.deliver_local(addr.id, make_bench_msg(MailboxLoadTag));
    }

    uint64_t expected_pongs =
        static_cast<uint64_t>(kActorsPerNode) * kActorsPerNode * kPingsPerTarget;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counters.pongs_received.load(std::memory_order_acquire) < expected_pongs &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    metrics.total_sent = counters.pings_sent.load();
    metrics.total_received = counters.pongs_received.load();
    metrics.completed = shutdown.has_value() &&
                        metrics.total_sent >= expected_pongs &&
                        metrics.total_received >= expected_pongs;
    metrics.throughput_msgps =
        throughput(metrics.total_received, metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

} // namespace hpactor::apps::bench_caf
