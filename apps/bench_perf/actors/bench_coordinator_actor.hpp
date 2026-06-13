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
// Benchmark Preset
// =============================================================================

struct BenchmarkPreset {
    std::string name;
    std::string description;
    uint32_t num_workers = 5000;
    uint64_t cold_burn_us = 10;
    uint32_t cold_rate_hz = 100;
    uint32_t num_hot_actors = 0;
    uint64_t hot_burn_us = 500;
    uint32_t hot_rate_hz = 1000;
    uint32_t duration_ms = 30000;
};

// =============================================================================
// BenchCoordinatorActor
// =============================================================================

/// \brief Orchestrates benchmark runs and reports results.
///
/// Holds preset configurations, dispatches parameterized start/stop messages
/// to workers and hot actors, and produces formatted reports via
/// serialize_state(). Responds to BenchStart/BenchStop/StatsPoll messages from
/// CLI commands.
class BenchCoordinatorActor : public EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "BenchCoordinatorActor";

    BenchCoordinatorActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys),
          epoch_start_(std::chrono::steady_clock::now()) {
        presets_.push_back({"many-actors",
                            "5000 workers, 10us burn, 100Hz — throughput test",
                            5000, 10, 100, 0, 500, 1000, 30000});
        presets_.push_back(
            {"hot-actor",
             "1 hot actor (500us, 1000Hz) + 1000 cold workers (10us, 10Hz) — fairness test",
             1000, 10, 10, 1, 500, 1000, 30000});
        become(make_behavior());
    }

    void set_worker_addrs(std::vector<ActorAddress> cold_addrs,
                          std::vector<ActorAddress> hot_addrs,
                          ActorAddress collector_addr) {
        cold_worker_addrs_ = std::move(cold_addrs);
        hot_worker_addrs_ = std::move(hot_addrs);
        collector_addr_ = collector_addr;
    }

    const std::vector<BenchmarkPreset>& presets() const {
        return presets_;
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "BenchCoordinatorActor";
        m.state = running_ ? "Running" : "Idle";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        std::ostringstream oss;
        oss << (running_ ? active_preset_ : "none");
        m.behavior_name = oss.str();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        return build_report_bytes();
    }

    bool is_running() const {
        return running_;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1, std::memory_order_relaxed);

            if (msg.type_id() == BenchStartTag) {
                handle_bench_start(msg);
            } else if (msg.type_id() == BenchStopTag) {
                handle_bench_stop();
            } else if (msg.type_id() == StatsPollTag) {
                handle_stats_poll();
            }
        }};
    }

  private:
    void handle_bench_start(TypedMessage& msg) {
        const auto& p = msg.payload();
        std::string preset_name(p.data(), p.data() + p.size());

        const BenchmarkPreset* preset = nullptr;
        for (auto& pr : presets_) {
            if (pr.name == preset_name) {
                preset = &pr;
                break;
            }
        }
        if (!preset) {
            last_error_ = "Unknown preset: " + preset_name;
            return;
        }

        // Guard against early CLI commands arriving before main() finishes
        // wiring
        if (cold_worker_addrs_.empty() && hot_worker_addrs_.empty()) {
            last_error_ =
                "Workers not yet initialized. Please wait a moment and try again.";
            return;
        }

        active_preset_ = preset->name;
        last_error_.clear();

        // Build BenchStart payload: {burn_us: uint64, rate_hz: uint32,
        // duration_ms: uint32}
        uint8_t start_buf[16];
        std::memset(start_buf, 0, sizeof(start_buf));

        // Send to cold workers with cold parameters
        {
            uint64_t burn = preset->cold_burn_us;
            uint32_t rate = preset->cold_rate_hz;
            uint32_t dur = preset->duration_ms;
            std::memcpy(start_buf, &burn, sizeof(uint64_t));
            std::memcpy(start_buf + 8, &rate, sizeof(uint32_t));
            std::memcpy(start_buf + 12, &dur, sizeof(uint32_t));
            StreamBuffer cold_payload(start_buf, start_buf + sizeof(start_buf));
            for (auto& addr : cold_worker_addrs_) {
                context()->send(addr, make_msg(BenchStartTag, cold_payload));
            }
        }

        // Send to hot actors with hot parameters
        {
            uint64_t burn = preset->hot_burn_us;
            uint32_t rate = preset->hot_rate_hz;
            uint32_t dur = preset->duration_ms;
            std::memcpy(start_buf, &burn, sizeof(uint64_t));
            std::memcpy(start_buf + 8, &rate, sizeof(uint32_t));
            std::memcpy(start_buf + 12, &dur, sizeof(uint32_t));
            StreamBuffer hot_payload(start_buf, start_buf + sizeof(start_buf));
            for (auto& addr : hot_worker_addrs_) {
                context()->send(addr, make_msg(BenchStartTag, hot_payload));
            }
        }

        // Start collector
        {
            StreamBuffer empty;
            context()->send(collector_addr_,
                            make_msg(BenchStartTag, std::move(empty)));
        }

        running_ = true;
        start_time_ = std::chrono::steady_clock::now();
    }

    void handle_bench_stop() {
        StreamBuffer empty;
        for (auto& addr : cold_worker_addrs_)
            context()->send(addr, make_msg(BenchStopTag, empty));
        for (auto& addr : hot_worker_addrs_)
            context()->send(addr, make_msg(BenchStopTag, empty));
        context()->send(collector_addr_, make_msg(BenchStopTag, empty));
        running_ = false;
    }

    void handle_stats_poll() {
        // Forward poll to collector; collector replies directly to sender
        StreamBuffer empty;
        context()->send(collector_addr_, make_msg(StatsPollTag, empty));
    }

    std::vector<uint8_t> build_report_bytes() const {
        std::ostringstream oss;
        oss << "preset=" << active_preset_ << "\n";
        oss << "running=" << (running_ ? "yes" : "no") << "\n";
        if (!last_error_.empty())
            oss << "error=" << last_error_ << "\n";
        if (running_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start_time_)
                               .count();
            oss << "elapsed_ms=" << elapsed << "\n";
        }
        oss << "cold_workers=" << cold_worker_addrs_.size() << "\n";
        oss << "hot_workers=" << hot_worker_addrs_.size() << "\n";
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    std::chrono::steady_clock::time_point epoch_start_;
    std::chrono::steady_clock::time_point start_time_;
    std::vector<BenchmarkPreset> presets_;
    ActorAddress collector_addr_;
    std::vector<ActorAddress> cold_worker_addrs_;
    std::vector<ActorAddress> hot_worker_addrs_;
    std::string active_preset_;
    std::string last_error_;
    std::atomic<uint64_t> processed_{0};
    bool running_ = false;
};

} // namespace hpactor::apps::bench_perf
