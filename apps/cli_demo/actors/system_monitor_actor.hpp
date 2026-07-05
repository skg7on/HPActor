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
#include <hpactor/cli/cli_types.hpp>

#include "../messages.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::cli_demo {

/// \brief Periodically gathers system-wide stats from all actors.
///
/// Every 2 seconds, iterates all actors via \c system().for_each_actor()
/// and aggregates message counts and actor states. Provides data visible
/// through \c /system stats and \c /actor &lt;id&gt; show.
class SystemMonitorActor : public EventBasedActor {
  public:
    SystemMonitorActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys),
          epoch_start_(std::chrono::steady_clock::now()) {
        become(make_behavior());
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "SystemMonitorActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "total_messages=" << total_messages_
            << " running_actors=" << running_actors_
            << " idle_actors=" << idle_actors_
            << " scheduler_utilization=" << scheduler_utilization_;
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == StartTag || msg.type_id() == PeriodicTickTag) {
                gather_stats();
            }
        }};
    }

  private:
    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    void gather_stats() {
        total_messages_ = 0;
        running_actors_ = 0;
        idle_actors_ = 0;

        system().for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
            auto meta = actor.to_metadata();
            total_messages_ += meta.messages_processed;
            if (meta.state == "Running")
                running_actors_++;
            else if (meta.state == "Idle")
                idle_actors_++;
        });

        // Approximate utilization from actor count and thread count
        auto* sched = system().scheduler();
        if (sched && sched->worker_count() > 0) {
            scheduler_utilization_ = static_cast<double>(running_actors_) /
                                     static_cast<double>(sched->worker_count() * 2);
        }

        context()->schedule(std::chrono::milliseconds(2000),
                            make_msg(PeriodicTickTag));
    }

    uint64_t total_messages_ = 0;
    uint32_t running_actors_ = 0;
    uint32_t idle_actors_ = 0;
    double scheduler_utilization_ = 0.0;
    std::atomic<uint64_t> processed_{0};
    std::chrono::steady_clock::time_point epoch_start_;
};

} // namespace hpactor::apps::cli_demo
