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
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::bench_saturate {

// =============================================================================
// SaturateReceiverActor — bounded mailbox, drop counting, latency extraction
// =============================================================================

class SaturateReceiverActor : public EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "SaturateReceiverActor";

    SaturateReceiverActor(ActorContext* ctx, ActorSystem& sys,
                          ActorAddress collector_addr, uint32_t receiver_index)
        : EventBasedActor(ctx, sys), collector_addr_(collector_addr),
          receiver_index_(receiver_index),
          epoch_start_(std::chrono::steady_clock::now()) {
        add_fast_tag(LoadMessageTag);
        become(make_behavior());
    }

    void set_collector_addr(ActorAddress addr) {
        collector_addr_ = addr;
    }

    uint64_t received_count() const {
        return received_count_.load();
    }
    uint64_t dropped_count() const {
        return dropped_count_.load();
    }
    uint32_t receiver_index() const {
        return receiver_index_;
    }
    bool is_running() const {
        return running_;
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = kActorTypeName;
        m.state = running_ ? "Running" : "Idle";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        std::ostringstream oss;
        oss << "saturate-receiver-" << receiver_index_;
        m.behavior_name = oss.str();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "receiver_index=" << receiver_index_
            << " received=" << received_count_.load()
            << " dropped=" << dropped_count_.load()
            << " running=" << (running_ ? "yes" : "no");
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1, std::memory_order_relaxed);

            if (msg.type_id() == LoadMessageTag) {
                handle_load_message(msg);
            } else if (msg.type_id() == SaturateStartTag) {
                handle_start();
            } else if (msg.type_id() == SaturateStopTag) {
                handle_stop();
            } else if (msg.type_id() == StatsPollTag) {
                handle_stats_poll();
            }
        }};
    }

  private:
    void handle_load_message(TypedMessage& msg) {
        if (!running_)
            return;

        received_count_.fetch_add(1, std::memory_order_relaxed);
        uint64_t rcvd = received_count_.load(std::memory_order_relaxed);

        // Sample 1% of messages for latency (statistically sufficient
        // at scale and avoids overwhelming the collector).
        if (rcvd % 100 == 0) {
            const auto& p = msg.payload();
            if (p.size() >= LoadMessagePayload::kHeaderSize) {
                auto decoded = LoadMessagePayload::decode(p);
                auto now = std::chrono::steady_clock::now();
                auto send_time = std::chrono::steady_clock::time_point(
                    std::chrono::microseconds(decoded.send_timestamp_us));
                uint32_t latency_us = static_cast<uint32_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(now - send_time)
                        .count());

                LatencySamplePayload sample;
                sample.sender_id = decoded.sender_id;
                sample.seq_no = decoded.seq_no;
                sample.latency_us = latency_us;
                context()->send(collector_addr_,
                                make_msg(LatencySampleTag, sample.encode()));
            }
        }

        if (rcvd % 1000 == 0) {
            send_drop_report();
        }
    }

    void handle_start() {
        received_count_.store(0);
        dropped_count_.store(0);
        running_ = true;
        start_time_ = std::chrono::steady_clock::now();
    }

    void handle_stop() {
        running_ = false;
        send_drop_report();
    }

    void handle_stats_poll() {
        send_drop_report();
    }

    void send_drop_report() {
        DropReportPayload report;
        report.receiver_id = id().value();
        report.total_received = received_count_.load();
        report.total_dropped = dropped_count_.load();
        context()->send(collector_addr_, make_msg(DropReportTag, report.encode()));
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    ActorAddress collector_addr_;
    uint32_t receiver_index_;
    std::chrono::steady_clock::time_point epoch_start_;
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<uint64_t> received_count_{0};
    std::atomic<uint64_t> dropped_count_{0};
    std::atomic<uint64_t> processed_{0};
    bool running_ = false;
};

} // namespace hpactor::apps::bench_saturate
