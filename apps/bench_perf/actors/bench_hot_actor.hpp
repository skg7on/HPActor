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

namespace hpactor::apps::bench_perf {

// =============================================================================
// BenchHotActor — heavy CPU burn, high message rate (group = 1)
// =============================================================================

/// \brief "Noisy neighbor" actor for fairness benchmarking.
///
/// Same self-scheduling pattern as BenchWorkerActor but configured with
/// significantly higher burn duration and message rate. Sends latency
/// samples with \c group = 1 (hot) to the collector.
class BenchHotActor : public EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "BenchHotActor";

    BenchHotActor(ActorContext* ctx, ActorSystem& sys,
                  ActorAddress collector_addr, uint32_t hot_index = 0)
        : EventBasedActor(ctx, sys), collector_addr_(collector_addr),
          hot_index_(hot_index), epoch_start_(std::chrono::steady_clock::now()) {
        become(make_behavior());
    }

    void set_burn_params(uint64_t burn_us, uint32_t rate_hz, uint32_t duration_ms) {
        burn_us_ = burn_us;
        rate_hz_ = rate_hz;
        duration_ms_ = duration_ms;
    }

    void set_collector_addr(ActorAddress addr) {
        collector_addr_ = addr;
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "BenchHotActor";
        m.state = running_ ? "Running" : "Idle";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        std::ostringstream oss;
        oss << "bench-hot-" << hot_index_;
        m.behavior_name = oss.str();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "hot_index=" << hot_index_ << " processed=" << processed_.load()
            << " burn_us=" << burn_us_ << " rate_hz=" << rate_hz_
            << " running=" << (running_ ? "yes" : "no");
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

    bool is_running() const {
        return running_;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1, std::memory_order_relaxed);

            if (msg.type_id() == BenchStartTag) {
                const auto& p = msg.payload();
                if (p.size() >= 16) {
                    const uint8_t* d = p.data();
                    uint64_t burn;
                    uint32_t rate, dur;
                    std::memcpy(&burn, d, sizeof(uint64_t));
                    std::memcpy(&rate, d + 8, sizeof(uint32_t));
                    std::memcpy(&dur, d + 12, sizeof(uint32_t));
                    set_burn_params(burn, rate, dur);
                }
                running_ = true;
                start_time_ = std::chrono::steady_clock::now();
                schedule_next();

            } else if (msg.type_id() == BenchStopTag) {
                running_ = false;

            } else if (msg.type_id() == PeriodicTickTag) {
                if (!running_)
                    return;
                do_tick();
            }
        }};
    }

  private:
    static constexpr TypeTag PeriodicTickTag{0x00010110};

    void schedule_next() {
        if (!running_)
            return;
        // schedule() takes milliseconds — compute interval in ms, minimum 1ms
        uint32_t interval_ms = (rate_hz_ > 0) ? (1000 / rate_hz_) : 1;
        if (interval_ms == 0)
            interval_ms = 1;
        context()->schedule(std::chrono::milliseconds(interval_ms),
                            make_msg(PeriodicTickTag));
    }

    void do_tick() {
        auto t0 = std::chrono::steady_clock::now();

        // Heavy CPU burn
        burn_cpu_us(burn_us_);

        auto t1 = std::chrono::steady_clock::now();
        uint64_t latency_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

        // Send latency sample to collector (group = 1 = hot)
        uint8_t payload_buf[17];
        uint64_t aid = id().value();
        uint8_t group = 1; // hot
        std::memcpy(payload_buf, &aid, sizeof(uint64_t));
        std::memcpy(payload_buf + 8, &latency_us, sizeof(uint64_t));
        payload_buf[16] = group;
        StreamBuffer payload(payload_buf, payload_buf + sizeof(payload_buf));
        context()->send(collector_addr_,
                        make_msg(LatencySampleTag, std::move(payload)));

        // Duration check
        if (duration_ms_ > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start_time_)
                               .count();
            if (elapsed >= static_cast<decltype(elapsed)>(duration_ms_)) {
                running_ = false;
                return;
            }
        }

        schedule_next();
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    ActorAddress collector_addr_;
    uint32_t hot_index_;
    std::chrono::steady_clock::time_point epoch_start_;
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<uint64_t> processed_{0};
    uint64_t burn_us_ = 500;
    uint32_t rate_hz_ = 1000;
    uint32_t duration_ms_ = 30000;
    bool running_ = false;
};

} // namespace hpactor::apps::bench_perf
