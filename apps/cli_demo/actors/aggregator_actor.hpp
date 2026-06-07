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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::cli_demo {

/// \brief Collects worker results and maintains aggregate statistics.
///
/// Receives \c WorkerResultTag messages from all workers and computes
/// running averages, p50, and p99 latency percentiles. Responds to
/// \c MonitorQueryTag with a snapshot of current aggregate stats.
class AggregatorActor : public EventBasedActor {
  public:
    AggregatorActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        latencies_.reserve(4096);
        become(make_behavior());
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "AggregatorActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = 0;
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "total_processed=" << total_processed_
            << " avg_latency_us=" << std::fixed << std::setprecision(1)
            << avg_latency_us_ << " p50_us=" << std::fixed << std::setprecision(1)
            << p50_us_ << " p99_us=" << std::fixed << std::setprecision(1)
            << p99_us_ << " active_workers=" << active_workers_;
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == WorkerResultTag) {
                if (msg.payload().size() >= 16) {
                    auto* data =
                        reinterpret_cast<const double*>(msg.payload().data());
                    double latency = data[0];

                    total_processed_++;
                    avg_latency_us_ = (avg_latency_us_ * 0.95) + (latency * 0.05);
                    latencies_.push_back(latency);

                    if (latencies_.size() >= 100) {
                        std::sort(latencies_.begin(), latencies_.end());
                        p50_us_ = latencies_[latencies_.size() / 2];
                        p99_us_ = latencies_[latencies_.size() * 99 / 100];
                        if (latencies_.size() > 2000) {
                            latencies_.erase(latencies_.begin(),
                                             latencies_.begin() + 1000);
                        }
                    }
                }
            } else if (msg.type_id() == MonitorQueryTag) {
                std::array<uint64_t, 5> stats = {
                    total_processed_, static_cast<uint64_t>(avg_latency_us_),
                    static_cast<uint64_t>(p50_us_),
                    static_cast<uint64_t>(p99_us_), active_workers_};
                StreamBuffer payload(
                    reinterpret_cast<const uint8_t*>(stats.data()),
                    reinterpret_cast<const uint8_t*>(stats.data() + 5));
                context()->reply(make_msg(MonitorReplyTag, std::move(payload)));
            }
        }};
    }

  private:
    uint64_t total_processed_ = 0;
    double avg_latency_us_ = 0.0;
    double p50_us_ = 0.0;
    double p99_us_ = 0.0;
    uint32_t active_workers_ = 4;
    std::vector<double> latencies_;
    std::atomic<uint64_t> processed_{0};
};

} // namespace hpactor::apps::cli_demo
