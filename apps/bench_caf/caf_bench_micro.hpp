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

#include "caf_bench_config.hpp"
#include "caf_bench_metrics.hpp"
#include "messages.hpp"

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

namespace hpactor::apps::bench_caf {

struct MicroResult {
    uint64_t iterations = 0;
    uint64_t runtime_ns = 0;
    uint64_t ops_per_sec = 0;
    uint64_t alloc_count = 0;
    uint64_t alloc_bytes = 0;
};

inline MicroResult
run_micro_benchmark(std::function<void()> setup, std::function<void()> op,
                    uint64_t op_count) {
    MicroResult result;
    result.iterations = op_count;
    if (op_count == 0)
        return result;

    setup();
    auto start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < op_count; ++i) {
        op();
    }
    auto end = std::chrono::steady_clock::now();

    auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    result.runtime_ns = static_cast<uint64_t>(ns);
    if (ns > 0) {
        result.ops_per_sec = static_cast<uint64_t>(
            static_cast<double>(op_count) * 1e9 / static_cast<double>(ns));
    }
    return result;
}

inline TrialMetrics
run_message_creation_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    constexpr uint64_t kOps = 100000;
    BenchPayloadHeader header{1, 2, 3};
    size_t payload_size = std::max(static_cast<size_t>(cfg.message_size_bytes),
                                   BenchPayloadHeader::kEncodedSize);
    uint64_t counter = 0;

    auto setup = [&] { counter = 0; };
    auto op = [&] {
        auto payload = encode_shaped_payload(header, cfg.message_size_bytes,
                                             cfg.message_shape, cfg.seed);
        auto msg = make_bench_msg(MailboxLoadTag, std::move(payload));
        ++counter;
    };

    auto result = run_micro_benchmark(setup, op, kOps);
    metrics.throughput_msgps = static_cast<double>(result.ops_per_sec);
    metrics.total_sent = result.iterations;
    metrics.total_received = result.iterations;
    metrics.bytes_sent = result.iterations * payload_size;
    metrics.runtime_ms = result.runtime_ns / 1000000;
    metrics.completed = (counter == kOps);
    return metrics;
}

inline TrialMetrics
run_dispatch_match_trial(const CafBenchConfig& /*cfg*/, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    constexpr uint32_t kMessages = 10000;
    std::atomic<uint64_t> count{0};

    Config system_cfg;
    system_cfg.scheduler_threads = 1;
    system_cfg.mailbox.default_capacity = kMessages + 1024;
    system_cfg.enable_network = false;
    system_cfg.enable_receptionist = false;
    system_cfg.cli.enabled = false;
    system_cfg.tracing.enabled = false;

    auto start = std::chrono::steady_clock::now();
    ActorSystem system(system_cfg);

    struct DispatchActor : public EventBasedActor {
        std::atomic<uint64_t>* c;
        DispatchActor(ActorContext* ctx, ActorSystem& sys,
                      std::atomic<uint64_t>* counter)
            : EventBasedActor(ctx, sys), c(counter) {
            become(make_behavior());
        }
        Behavior make_behavior() override {
            return Behavior{[this](TypedMessage& msg) {
                if (msg.type_id() == MailboxLoadTag)
                    c->fetch_add(1, std::memory_order_relaxed);
            }};
        }
    };

    auto actor = system.spawn<DispatchActor>(&count);
    for (uint32_t i = 0; i < kMessages; ++i) {
        system.deliver_local(actor.id(), make_bench_msg(MailboxLoadTag));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (count.load(std::memory_order_acquire) < kMessages &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();

    auto runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
    metrics.runtime_ms = runtime_ms;
    metrics.total_sent = kMessages;
    metrics.total_received = count.load();
    metrics.completed = shutdown.has_value() && metrics.total_received >= kMessages;
    metrics.throughput_msgps = throughput(metrics.total_received, runtime_ms);
    return metrics;
}

inline TrialMetrics
run_serialization_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    constexpr uint64_t kOps = 50000;
    BenchPayloadHeader header{42, 100, 999};

    // Encode benchmark
    uint64_t encode_count = 0;
    auto encode_setup = [&] { encode_count = 0; };
    auto encode_op = [&] {
        auto payload = encode_shaped_payload(header, cfg.message_size_bytes,
                                             cfg.message_shape, cfg.seed);
        ++encode_count;
    };
    auto encode_result = run_micro_benchmark(encode_setup, encode_op, kOps);

    // Decode benchmark
    auto payload = encode_shaped_payload(header, cfg.message_size_bytes,
                                         cfg.message_shape, cfg.seed);
    uint64_t decode_count = 0;
    auto decode_setup = [&] { decode_count = 0; };
    auto decode_op = [&] {
        volatile auto hdr = decode_bench_payload(payload);
        (void)hdr;
        ++decode_count;
    };
    auto decode_result = run_micro_benchmark(decode_setup, decode_op, kOps);

    metrics.throughput_msgps = static_cast<double>(encode_result.ops_per_sec);
    metrics.total_sent = encode_result.iterations;
    metrics.total_received = decode_result.iterations;
    metrics.bytes_sent = encode_result.iterations * payload.size();
    metrics.runtime_ms =
        (encode_result.runtime_ns + decode_result.runtime_ns) / 1000000;
    metrics.completed = (encode_count == kOps && decode_count == kOps);
    return metrics;
}

} // namespace hpactor::apps::bench_caf
