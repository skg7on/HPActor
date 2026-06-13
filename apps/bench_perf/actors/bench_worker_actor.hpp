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
// BenchWorkerActor — light CPU burn, self-scheduling, latency sampling
// =============================================================================

/// \brief Cold/normal worker actor for throughput benchmarking.
///
/// Self-schedules via \c context()->schedule(), burns CPU inline for a
/// configurable duration, and sends latency + throughput samples to the
/// collector. Group = 0 (cold).
class BenchWorkerActor : public EventBasedActor {
  public:
    BenchWorkerActor(ActorContext* ctx, ActorSystem& sys,
                     ActorAddress collector_addr, uint32_t worker_index)
        : EventBasedActor(ctx, sys), collector_addr_(collector_addr),
          worker_index_(worker_index),
          epoch_start_(std::chrono::steady_clock::now()) {
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
        m.actor_type = "BenchWorkerActor";
        m.state = running_ ? "Running" : "Idle";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        std::ostringstream oss;
        oss << "bench-worker-" << worker_index_;
        m.behavior_name = oss.str();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "worker_index=" << worker_index_
            << " processed=" << processed_.load() << " burn_us=" << burn_us_
            << " rate_hz=" << rate_hz_
            << " running=" << (running_ ? "yes" : "no");
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

    uint32_t worker_index() const {
        return worker_index_;
    }
    bool is_running() const {
        return running_;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1, std::memory_order_relaxed);

            if (msg.type_id() == BenchStartTag) {
                // Payload: {burn_us: uint64, rate_hz: uint32, duration_ms:
                // uint32}
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
    /// \brief Internal tick tag (not in messages.hpp -- local to worker/hot
    /// actor).
    static constexpr TypeTag PeriodicTickTag{0x00010110};

    void schedule_next() {
        if (!running_)
            return;
        // schedule() takes milliseconds — compute interval in ms, minimum 1ms
        uint32_t interval_ms = (rate_hz_ > 0) ? (1000 / rate_hz_) : 10;
        if (interval_ms == 0)
            interval_ms = 1;
        context()->schedule(std::chrono::milliseconds(interval_ms),
                            make_msg(PeriodicTickTag));
    }

    void do_tick() {
        auto t0 = std::chrono::steady_clock::now();

        // CPU burn
        burn_cpu_us(burn_us_);

        auto t1 = std::chrono::steady_clock::now();
        uint64_t latency_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

        // Send latency sample to collector (group = 0 = cold)
        uint8_t payload_buf[17];
        uint64_t aid = id().value();
        uint8_t group = 0;
        std::memcpy(payload_buf, &aid, sizeof(uint64_t));
        std::memcpy(payload_buf + 8, &latency_us, sizeof(uint64_t));
        payload_buf[16] = group;
        StreamBuffer payload(payload_buf, payload_buf + sizeof(payload_buf));
        context()->send(collector_addr_,
                        make_msg(LatencySampleTag, std::move(payload)));

        // Check duration
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
    uint32_t worker_index_;
    std::chrono::steady_clock::time_point epoch_start_;
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<uint64_t> processed_{0};
    uint64_t burn_us_ = 10;
    uint32_t rate_hz_ = 100;
    uint32_t duration_ms_ = 30000;
    bool running_ = false;
};

} // namespace hpactor::apps::bench_perf
