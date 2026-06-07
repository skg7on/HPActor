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
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::cli_demo {

/// \brief Maintains logical time and responds to time queries.
///
/// Demonstrates the request-response pattern: receives \c TimeQueryTag and
/// replies with \c TimeReplyTag, returning the elapsed microseconds since
/// the actor's epoch. Also periodically updates its logical clock.
class ClockActor : public EventBasedActor {
  public:
    ClockActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        epoch_start_ = std::chrono::steady_clock::now();
        become(make_behavior());
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "ClockActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "logical_time_us=" << logical_time_us_
            << " queries_answered=" << queries_answered_
            << " uptime_ms=" << elapsed_ms();
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == TimeQueryTag) {
                auto now = std::chrono::steady_clock::now();
                logical_time_us_ =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        now - epoch_start_)
                        .count();
                queries_answered_++;
                context()->reply(make_msg(
                    TimeReplyTag,
                    encode_u64(static_cast<uint64_t>(logical_time_us_))));
            } else if (msg.type_id() == PeriodicTickTag) {
                logical_time_us_ =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - epoch_start_)
                        .count();
                context()->schedule(std::chrono::milliseconds(100),
                                    make_msg(PeriodicTickTag));
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

    std::chrono::steady_clock::time_point epoch_start_;
    int64_t logical_time_us_ = 0;
    uint64_t queries_answered_ = 0;
    std::atomic<uint64_t> processed_{0};
};

} // namespace hpactor::apps::cli_demo
