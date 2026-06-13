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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::bench_perf {

// =============================================================================
// BenchCollectorActor — streaming latency percentile + throughput stats
// =============================================================================

/// \brief Receives latency samples and throughput ticks from worker/hot actors,
///        computes streaming percentiles via reservoir sampling, and serves
///        stats snapshots on demand.
///
/// Reservoir: keeps the last 10,000 latency samples per group. On snapshot,
/// sorts and extracts p50/p99/p999. Throughput is computed via total samples
/// divided by elapsed seconds.
class BenchCollectorActor : public EventBasedActor {
  public:
    explicit BenchCollectorActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys),
          epoch_start_(std::chrono::steady_clock::now()) {
        hot_latencies_.reserve(kReservoirSize);
        cold_latencies_.reserve(kReservoirSize);
        become(make_behavior());
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "BenchCollectorActor";
        m.state = running_ ? "Collecting" : "Idle";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        return build_report_vec();
    }

    // Accessors used by coordinator for direct report formatting
    uint64_t total_hot_samples() const {
        return hot_count_.load();
    }
    uint64_t total_cold_samples() const {
        return cold_count_.load();
    }
    double hot_p50_us() const {
        return hot_p50_us_;
    }
    double hot_p99_us() const {
        return hot_p99_us_;
    }
    double hot_p999_us() const {
        return hot_p999_us_;
    }
    double cold_p50_us() const {
        return cold_p50_us_;
    }
    double cold_p99_us() const {
        return cold_p99_us_;
    }
    double cold_p999_us() const {
        return cold_p999_us_;
    }
    double hot_throughput_msgps() const {
        return hot_throughput_msgps_;
    }
    double cold_throughput_msgps() const {
        return cold_throughput_msgps_;
    }
    bool is_running() const {
        return running_;
    }
    uint64_t elapsed_run_ms() const {
        if (!running_ && run_start_ == decltype(run_start_){})
            return 0;
        auto end = running_ ? std::chrono::steady_clock::now() : run_end_;
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - run_start_)
                .count());
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);

            if (msg.type_id() == LatencySampleTag) {
                handle_latency_sample(msg);
            } else if (msg.type_id() == ThroughputTickTag) {
                handle_throughput_tick(msg);
            } else if (msg.type_id() == BenchStartTag) {
                handle_bench_start();
            } else if (msg.type_id() == BenchStopTag) {
                handle_bench_stop();
            } else if (msg.type_id() == StatsPollTag) {
                handle_stats_poll();
            }
        }};
    }

  private:
    static constexpr size_t kReservoirSize = 10000;

    void handle_latency_sample(TypedMessage& msg) {
        // Payload: {actor_id: uint64, latency_us: uint64, group: uint8}
        //  group: 0 = cold (worker), 1 = hot
        const auto& p = msg.payload();
        if (p.size() < 17)
            return; // 8 + 8 + 1
        const uint8_t* d = p.data();
        uint64_t latency_us;
        std::memcpy(&latency_us, d + 8, sizeof(uint64_t));
        uint8_t group = d[16];

        if (group == 0) {
            cold_latencies_.push_back(static_cast<double>(latency_us));
            if (cold_latencies_.size() > kReservoirSize)
                cold_latencies_.erase(
                    cold_latencies_.begin(),
                    cold_latencies_.begin() +
                        static_cast<ptrdiff_t>(cold_latencies_.size() -
                                               kReservoirSize));
            cold_count_.fetch_add(1, std::memory_order_relaxed);
        } else {
            hot_latencies_.push_back(static_cast<double>(latency_us));
            if (hot_latencies_.size() > kReservoirSize)
                hot_latencies_.erase(
                    hot_latencies_.begin(),
                    hot_latencies_.begin() +
                        static_cast<ptrdiff_t>(hot_latencies_.size() -
                                               kReservoirSize));
            hot_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void handle_throughput_tick(TypedMessage& /*msg*/) {
        // Throughput is computed from sample count / elapsed time on snapshot.
        // Individual tick handling is a no-op for now.
    }

    void handle_bench_start() {
        hot_latencies_.clear();
        cold_latencies_.clear();
        hot_count_.store(0);
        cold_count_.store(0);
        hot_p50_us_ = hot_p99_us_ = hot_p999_us_ = 0.0;
        cold_p50_us_ = cold_p99_us_ = cold_p999_us_ = 0.0;
        hot_throughput_msgps_ = 0.0;
        cold_throughput_msgps_ = 0.0;
        running_ = true;
        run_start_ = std::chrono::steady_clock::now();
    }

    void handle_bench_stop() {
        running_ = false;
        run_end_ = std::chrono::steady_clock::now();
        recompute_percentiles();
    }

    void handle_stats_poll() {
        recompute_percentiles();
        auto buf = build_report();
        context()->reply(make_msg(StatsReplyTag, std::move(buf)));
    }

    void recompute_percentiles() {
        if (!hot_latencies_.empty()) {
            std::vector<double> sorted(hot_latencies_);
            std::sort(sorted.begin(), sorted.end());
            hot_p50_us_ = sorted[sorted.size() / 2];
            hot_p99_us_ = sorted[sorted.size() * 99 / 100];
            hot_p999_us_ = sorted[sorted.size() * 999 / 1000];
        }
        if (!cold_latencies_.empty()) {
            std::vector<double> sorted(cold_latencies_);
            std::sort(sorted.begin(), sorted.end());
            cold_p50_us_ = sorted[sorted.size() / 2];
            cold_p99_us_ = sorted[sorted.size() * 99 / 100];
            cold_p999_us_ = sorted[sorted.size() * 999 / 1000];
        }
        // Throughput: samples / elapsed seconds
        double elapsed_s = static_cast<double>(elapsed_run_ms()) / 1000.0;
        if (elapsed_s > 0.0) {
            hot_throughput_msgps_ =
                static_cast<double>(hot_count_.load()) / elapsed_s;
            cold_throughput_msgps_ =
                static_cast<double>(cold_count_.load()) / elapsed_s;
        }
    }

    StreamBuffer build_report() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        oss << "running=" << (running_ ? "yes" : "no") << "\n";
        oss << "elapsed_ms=" << elapsed_run_ms() << "\n";
        oss << "hot_samples=" << hot_count_.load() << "\n";
        oss << "cold_samples=" << cold_count_.load() << "\n";
        oss << "hot_p50_us=" << hot_p50_us_ << "\n";
        oss << "hot_p99_us=" << hot_p99_us_ << "\n";
        oss << "hot_p999_us=" << hot_p999_us_ << "\n";
        oss << "cold_p50_us=" << cold_p50_us_ << "\n";
        oss << "cold_p99_us=" << cold_p99_us_ << "\n";
        oss << "cold_p999_us=" << cold_p999_us_ << "\n";
        oss << "hot_throughput_msgps=" << hot_throughput_msgps_ << "\n";
        oss << "cold_throughput_msgps=" << cold_throughput_msgps_ << "\n";
        oss << "total_throughput_msgps="
            << (hot_throughput_msgps_ + cold_throughput_msgps_) << "\n";
        auto s = oss.str();
        return StreamBuffer(s.begin(), s.end());
    }

    std::vector<uint8_t> build_report_vec() const {
        auto buf = build_report();
        return {buf.begin(), buf.end()};
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    std::chrono::steady_clock::time_point epoch_start_;
    std::chrono::steady_clock::time_point run_start_;
    std::chrono::steady_clock::time_point run_end_;
    std::vector<double> hot_latencies_;
    std::vector<double> cold_latencies_;
    std::atomic<uint64_t> hot_count_{0};
    std::atomic<uint64_t> cold_count_{0};
    double hot_p50_us_ = 0.0, hot_p99_us_ = 0.0, hot_p999_us_ = 0.0;
    double cold_p50_us_ = 0.0, cold_p99_us_ = 0.0, cold_p999_us_ = 0.0;
    double hot_throughput_msgps_ = 0.0;
    double cold_throughput_msgps_ = 0.0;
    std::atomic<uint64_t> processed_{0};
    bool running_ = false;
};

} // namespace hpactor::apps::bench_perf
