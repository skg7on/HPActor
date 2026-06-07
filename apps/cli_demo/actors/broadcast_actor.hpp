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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/cli/cli_types.hpp>

#include "../messages.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::cli_demo {

/// \brief Periodically broadcasts config updates to all workers.
///
/// Every 1 second sends \c BroadcastConfigTag messages to all registered
/// workers. Demonstrates fan-out messaging and periodic scheduling.
class BroadcastActor : public EventBasedActor {
  public:
    BroadcastActor(ActorContext* ctx, ActorSystem& sys)
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
        m.actor_type = "BroadcastActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "broadcasts_sent=" << broadcasts_sent_
            << " target_workers=" << workers_.size();
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == StartTag || msg.type_id() == PeriodicTickTag) {
                do_broadcast();
            }
        }};
    }

  private:
    void do_broadcast() {
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "config:v%lu:batch_size=%u",
                         static_cast<unsigned long>(broadcasts_sent_),
                         static_cast<unsigned>(16 + (broadcasts_sent_ % 3) * 8));
        StreamBuffer payload(reinterpret_cast<const uint8_t*>(buf),
                             reinterpret_cast<const uint8_t*>(buf + n));
        for (auto& addr : workers_) {
            context()->send(addr, make_msg(BroadcastConfigTag, payload));
        }
        broadcasts_sent_++;
        context()->schedule(std::chrono::milliseconds(1000),
                            make_msg(PeriodicTickTag));
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    std::vector<ActorAddress> workers_;
    uint64_t broadcasts_sent_ = 0;
    std::atomic<uint64_t> processed_{0};
    std::chrono::steady_clock::time_point epoch_start_;
};

} // namespace hpactor::apps::cli_demo
