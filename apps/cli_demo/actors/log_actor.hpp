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
#include <hpactor/actor/stateful_actor.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/cli/cli_types.hpp>

#include "../messages.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::cli_demo {

/// \brief Ring-buffer event log stored as actor state.
struct LogRingBuffer {
    static constexpr size_t kCapacity = 256;
    std::deque<std::string> entries;

    void append(const std::string& entry) {
        if (entries.size() >= kCapacity)
            entries.pop_front();
        entries.push_back(entry);
    }

    size_t size() const {
        return entries.size();
    }
};

/// \brief Ring-buffer event log actor using \c StatefulActor<T>.
///
/// Receives \c LogEntryTag messages from workers on milestone events
/// and stores them in a fixed-capacity ring buffer. State serialization
/// exposes the most recent entries for CLI inspection via /actor show.
class LogActor : public StatefulActor<LogRingBuffer> {
  public:
    LogActor(ActorContext* ctx, ActorSystem& sys)
        : StatefulActor<LogRingBuffer>(ctx, sys),
          epoch_start_(std::chrono::steady_clock::now()) {
        become(make_behavior());
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "LogActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        auto& buf = state();
        oss << "total_events=" << total_events_ << " ring_depth=" << buf.size()
            << " capacity=" << LogRingBuffer::kCapacity;
        if (!buf.entries.empty()) {
            oss << "\n  last_5_events:";
            size_t start = buf.entries.size() > 5 ? buf.entries.size() - 5 : 0;
            for (size_t i = start; i < buf.entries.size(); ++i) {
                oss << "\n    " << buf.entries[i];
            }
        }
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == LogEntryTag) {
                std::string entry(msg.payload().begin(), msg.payload().end());
                state().append(entry);
                total_events_++;
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

    uint64_t total_events_ = 0;
    std::atomic<uint64_t> processed_{0};
    std::chrono::steady_clock::time_point epoch_start_;
};

} // namespace hpactor::apps::cli_demo
