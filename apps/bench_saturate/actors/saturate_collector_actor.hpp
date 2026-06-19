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
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::apps::bench_saturate {

// =============================================================================
// SaturateCollectorActor — streaming percentile + drop-curve aggregation
// =============================================================================

class SaturateCollectorActor : public EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "SaturateCollectorActor";

    explicit SaturateCollectorActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys),
          epoch_start_(std::chrono::steady_clock::now()) {
        drop_curve_.reserve(1024);
        become(make_behavior());
    }

    uint64_t total_sent() const {
        return total_sent_.load();
    }
    uint64_t total_received() const {
        return total_received_.load();
    }
    uint64_t total_dropped() const {
        return total_dropped_.load();
    }
    double drop_rate_pct() const {
        return drop_rate_pct_;
    }
    double p50_us() const {
        return p50_us_;
    }
    double p99_us() const {
        return p99_us_;
    }
    double p999_us() const {
        return p999_us_;
    }
    double throughput_msgps() const {
        return throughput_msgps_;
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

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = kActorTypeName;
        m.state = running_ ? "Collecting" : "Idle";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        const_cast<SaturateCollectorActor*>(this)->recompute_percentiles();
        return build_report_vec();
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1, std::memory_order_relaxed);

            if (msg.type_id() == ThroughputSampleTag) {
                handle_throughput_sample(msg);
            } else if (msg.type_id() == LatencySampleTag) {
                handle_latency_sample(msg);
            } else if (msg.type_id() == DropReportTag) {
                handle_drop_report(msg);
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
    static constexpr size_t kReservoirSize = 10000;

    void handle_throughput_sample(TypedMessage& msg) {
        const auto& p = msg.payload();
        if (p.size() >= 20) {
            auto sample = ThroughputSamplePayload::decode(p);

            uint64_t prev_sent = last_sender_sent_[sample.sender_id];
            if (sample.total_sent > prev_sent) {
                total_sent_.fetch_add(sample.total_sent - prev_sent,
                                      std::memory_order_relaxed);
                last_sender_sent_[sample.sender_id] = sample.total_sent;
            }

            uint64_t prev_drop = last_sender_dropped_[sample.sender_id];
            if (sample.send_dropped > prev_drop) {
                total_dropped_.fetch_add(sample.send_dropped - prev_drop,
                                         std::memory_order_relaxed);
                last_sender_dropped_[sample.sender_id] = sample.send_dropped;
            }
        }
        if (running_ && drop_curve_.size() < 1024) {
            drop_curve_.push_back(
                {static_cast<uint64_t>(
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - run_start_)
                         .count()),
                 drop_rate_pct_});
        }
    }

    void handle_latency_sample(TypedMessage& msg) {
        const auto& p = msg.payload();
        if (p.size() < 16)
            return;
        LatencySamplePayload sample = LatencySamplePayload::decode(p);

        // Lock-free ring buffer: atomic write index wraps, overwriting
        // oldest samples once capacity is reached. O(1) per insert.
        size_t idx = latency_write_count_.fetch_add(1, std::memory_order_relaxed) %
                     kReservoirSize;
        latencies_[idx].store(static_cast<double>(sample.latency_us),
                              std::memory_order_relaxed);
    }

    void handle_drop_report(TypedMessage& msg) {
        const auto& p = msg.payload();
        if (p.size() < 24)
            return;
        DropReportPayload report = DropReportPayload::decode(p);

        // Track per-receiver deltas for accurate received + dropped counts.
        uint64_t prev_recv = last_receiver_received_[report.receiver_id];
        uint64_t prev_drop = last_receiver_dropped_[report.receiver_id];

        if (report.total_received > prev_recv) {
            total_received_.fetch_add(report.total_received - prev_recv,
                                      std::memory_order_relaxed);
            last_receiver_received_[report.receiver_id] = report.total_received;
        }
        if (report.total_dropped > prev_drop) {
            total_dropped_.fetch_add(report.total_dropped - prev_drop,
                                     std::memory_order_relaxed);
            last_receiver_dropped_[report.receiver_id] = report.total_dropped;
        }
    }

    void handle_start() {
        latency_write_count_.store(0, std::memory_order_relaxed);
        drop_curve_.clear();
        last_sender_sent_.clear();
        last_sender_dropped_.clear();
        last_receiver_received_.clear();
        last_receiver_dropped_.clear();
        total_sent_.store(0);
        total_received_.store(0);
        total_dropped_.store(0);
        p50_us_ = p99_us_ = p999_us_ = 0.0;
        drop_rate_pct_ = 0.0;
        throughput_msgps_ = 0.0;
        running_ = true;
        run_start_ = std::chrono::steady_clock::now();
    }

    void handle_stop() {
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
        // Snapshot from lock-free ring buffer.
        size_t total = latency_write_count_.load(std::memory_order_acquire);
        size_t count = total < kReservoirSize ? total : kReservoirSize;
        if (count > 0) {
            std::vector<double> sorted;
            sorted.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                sorted.push_back(latencies_[i].load(std::memory_order_relaxed));
            }
            std::sort(sorted.begin(), sorted.end());
            p50_us_ = sorted[count / 2];
            p99_us_ = sorted[count * 99 / 100];
            p999_us_ = sorted[count * 999 / 1000];
        }

        uint64_t received = total_received_.load();
        uint64_t dropped = total_dropped_.load();
        uint64_t total_delivery = received + dropped;
        if (total_delivery > 0) {
            drop_rate_pct_ = 100.0 * static_cast<double>(dropped) /
                             static_cast<double>(total_delivery);
        } else {
            drop_rate_pct_ = 0.0;
        }

        double elapsed_s = static_cast<double>(elapsed_run_ms()) / 1000.0;
        if (elapsed_s > 0.0) {
            throughput_msgps_ = static_cast<double>(received) / elapsed_s;
        }
    }

    StreamBuffer build_report() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "running=" << (running_ ? "yes" : "no") << "\n";
        oss << "elapsed_ms=" << elapsed_run_ms() << "\n";
        oss << "total_sent=" << total_sent_.load() << "\n";
        oss << "total_received=" << total_received_.load() << "\n";
        oss << "total_dropped=" << total_dropped_.load() << "\n";
        oss << "drop_rate_pct=" << drop_rate_pct_ << "\n";
        oss << "p50_us=" << p50_us_ << "\n";
        oss << "p99_us=" << p99_us_ << "\n";
        oss << "p999_us=" << p999_us_ << "\n";
        oss << "throughput_msgps=" << throughput_msgps_ << "\n";
        oss << "curve_points=" << drop_curve_.size() << "\n";
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

    struct CurvePoint {
        uint64_t elapsed_ms;
        double drop_rate_pct;
    };

    std::chrono::steady_clock::time_point epoch_start_;
    std::chrono::steady_clock::time_point run_start_;
    std::chrono::steady_clock::time_point run_end_;
    // Lock-free ring buffer for latency samples: fixed-size array +
    // atomic write count. Writes wrap modulo kReservoirSize.
    std::array<std::atomic<double>, kReservoirSize> latencies_{};
    std::atomic<size_t> latency_write_count_{0};
    std::vector<CurvePoint> drop_curve_;
    std::atomic<uint64_t> total_sent_{0};
    std::atomic<uint64_t> total_received_{0};
    std::atomic<uint64_t> total_dropped_{0};
    std::unordered_map<uint64_t, uint64_t> last_sender_sent_;
    std::unordered_map<uint64_t, uint64_t> last_sender_dropped_;
    std::unordered_map<uint64_t, uint64_t> last_receiver_received_;
    std::unordered_map<uint64_t, uint64_t> last_receiver_dropped_;
    double p50_us_ = 0.0, p99_us_ = 0.0, p999_us_ = 0.0;
    double drop_rate_pct_ = 0.0;
    double throughput_msgps_ = 0.0;
    std::atomic<uint64_t> processed_{0};
    bool running_ = false;
};

} // namespace hpactor::apps::bench_saturate
