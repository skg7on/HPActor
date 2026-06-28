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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>

#include "../caf_bench_config.hpp"
#include "../messages.hpp"

#include <atomic>
#include <cstdint>

namespace hpactor::apps::bench_caf {

struct MixedCaseCounters {
    std::atomic<uint64_t> actors_created{0};
    std::atomic<uint64_t> rings_completed{0};
    std::atomic<uint64_t> token_hops{0};
    std::atomic<uint64_t> cpu_tasks_completed{0};
};

struct MixedCaseDimensions {
    uint32_t rings = 4;
    uint32_t ring_size = 16;
    uint32_t token_value = 100;
    uint32_t repetitions = 1;
};

inline MixedCaseDimensions mixed_case_dimensions_for_preset(PresetKind preset) {
    switch (preset) {
        case PresetKind::Smoke:
            return {4, 16, 25, 1};
        case PresetKind::Nightly:
            return {16, 64, 500, 1};
        case PresetKind::PaperScale:
            return {100, 100, 1000, 4};
        case PresetKind::Stress:
            return {128, 128, 2000, 4};
    }
    return {4, 16, 100, 1};
}

inline uint64_t factorization_work(uint64_t value) {
    uint64_t factors = 0;
    for (uint64_t candidate = 2; candidate * candidate <= value; ++candidate) {
        while (value % candidate == 0) {
            value /= candidate;
            ++factors;
        }
    }
    if (value > 1)
        ++factors;
    return factors;
}

class MixedCpuActor : public EventBasedActor {
  public:
    MixedCpuActor(ActorContext* ctx, ActorSystem& sys, MixedCaseCounters* counters)
        : EventBasedActor(ctx, sys), counters_(counters) {
        counters_->actors_created.fetch_add(1, std::memory_order_relaxed);
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != MixedCpuTaskTag)
                return;
            volatile auto factors = factorization_work(86028157ULL);
            (void)factors;
            counters_->cpu_tasks_completed.fetch_add(1, std::memory_order_release);
        }};
    }

  private:
    MixedCaseCounters* counters_ = nullptr;
};

class MixedRingNodeActor : public EventBasedActor {
  public:
    MixedRingNodeActor(ActorContext* ctx, ActorSystem& sys,
                       MixedCaseCounters* counters, ActorAddress next)
        : EventBasedActor(ctx, sys), counters_(counters), next_(next) {
        counters_->actors_created.fetch_add(1, std::memory_order_relaxed);
        become(make_behavior());
    }

    void set_next(ActorAddress next) {
        next_ = next;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != MixedTokenTag)
                return;
            counters_->token_hops.fetch_add(1, std::memory_order_relaxed);
            auto remaining = decode_bench_payload(msg.payload()).sequence;
            if (remaining == 0) {
                counters_->rings_completed.fetch_add(1, std::memory_order_release);
                return;
            }
            BenchPayloadHeader header;
            header.sequence = remaining - 1;
            context()->send(next_,
                            make_bench_msg(MixedTokenTag,
                                           encode_bench_payload(header, 0, 1)));
        }};
    }

  private:
    MixedCaseCounters* counters_ = nullptr;
    ActorAddress next_;
};

} // namespace hpactor::apps::bench_caf
