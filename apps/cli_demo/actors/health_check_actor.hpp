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
#include <hpactor/cli/cli_types.hpp>

#include "../messages.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::cli_demo {

/// \brief Periodically pings workers and tracks their health status.
///
/// Every 500ms sends \c HealthPingTag to all registered workers.
/// Workers respond with \c HealthPongTag containing their current load.
/// Tracks healthy vs unhealthy worker counts for CLI observability.
class HealthCheckActor : public EventBasedActor {
  public:
    HealthCheckActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys),
          epoch_start_(std::chrono::steady_clock::now()) {
        become(make_behavior());
    }

    void add_worker(ActorAddress addr) {
        workers_.push_back(addr);
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "HealthCheckActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "healthy_count=" << healthy_count_
            << " unhealthy_count=" << unhealthy_count_
            << " workers_tracked=" << workers_.size();
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == StartTag || msg.type_id() == PeriodicTickTag) {
                do_health_check();
            } else if (msg.type_id() == HealthPongTag) {
                healthy_count_++;
                unhealthy_count_ =
                    static_cast<uint32_t>(workers_.size()) - healthy_count_;
            }
        }};
    }

  private:
    void do_health_check() {
        healthy_count_ = 0;
        for (auto& addr : workers_) {
            context()->send(addr, make_msg(HealthPingTag));
        }
        context()->schedule(std::chrono::milliseconds(500),
                            make_msg(PeriodicTickTag));
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    std::vector<ActorAddress> workers_;
    uint32_t healthy_count_ = 0;
    uint32_t unhealthy_count_ = 0;
    std::atomic<uint64_t> processed_{0};
    std::chrono::steady_clock::time_point epoch_start_;
};

} // namespace hpactor::apps::cli_demo
