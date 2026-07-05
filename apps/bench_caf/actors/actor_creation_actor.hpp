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
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>

#include "../caf_bench_config.hpp"
#include "../messages.hpp"

#include <atomic>
#include <cstdint>

namespace hpactor::apps::bench_caf {

struct ActorCreationCounters {
    std::atomic<uint64_t> created{0};
    std::atomic<uint64_t> completed{0};
};

class ActorCreationNodeActor : public EventBasedActor {
  public:
    ActorCreationNodeActor(ActorContext* ctx, ActorSystem& sys,
                           ActorCreationCounters* counters, uint32_t depth)
        : EventBasedActor(ctx, sys), counters_(counters), depth_(depth) {
        counters_->created.fetch_add(1, std::memory_order_relaxed);
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != ActorCreationStartTag)
                return;
            run_node();
        }};
    }

  private:
    void run_node() {
        if (depth_ == 0) {
            counters_->completed.fetch_add(1, std::memory_order_release);
            return;
        }

        auto left = system().spawn<ActorCreationNodeActor>(counters_, depth_ - 1);
        auto right = system().spawn<ActorCreationNodeActor>(counters_, depth_ - 1);
        system().deliver_local(left.id(), make_bench_msg(ActorCreationStartTag));
        system().deliver_local(right.id(), make_bench_msg(ActorCreationStartTag));
        counters_->completed.fetch_add(1, std::memory_order_release);
    }

    ActorCreationCounters* counters_ = nullptr;
    uint32_t depth_ = 0;
};

inline uint32_t actor_creation_depth_for_preset(PresetKind preset) {
    switch (preset) {
        case PresetKind::Smoke:
            return 10;
        case PresetKind::Nightly:
            return 16;
        case PresetKind::PaperScale:
            return 20;
        case PresetKind::Stress:
            return 21;
    }
    return 10;
}

inline uint64_t actor_creation_expected_count(uint32_t depth) {
    return (uint64_t{1} << (depth + 1)) - 1;
}

} // namespace hpactor::apps::bench_caf
