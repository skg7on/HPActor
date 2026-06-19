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
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::bench_saturate {

// =============================================================================
// Ramp phase enum
// =============================================================================

enum class RampPhase : uint8_t {
    Idle = 0,
    Probing,
    Refining,
    Stable,
    Reporting
};

inline const char* phase_name(RampPhase p) {
    switch (p) {
        case RampPhase::Idle:
            return "Idle";
        case RampPhase::Probing:
            return "Probing";
        case RampPhase::Refining:
            return "Refining";
        case RampPhase::Stable:
            return "Stable";
        case RampPhase::Reporting:
            return "Reporting";
    }
    return "Unknown";
}

// =============================================================================
// Benchmark Preset
// =============================================================================

struct SaturatePreset {
    std::string name;
    std::string description;
    uint32_t num_senders = 100;
    uint32_t num_receivers = 10;
    PayloadMode payload_mode = PayloadMode::Small;
    uint16_t payload_size_min = 16;
    uint16_t payload_size_max = 16;
    uint32_t initial_rate_msgps = 100;
    uint16_t step_interval_ms = 1000;
    float drop_threshold_pct = 1.0f;
    uint8_t refine_iterations = 5;
    uint32_t mailbox_capacity = 4096;
    uint32_t stable_duration_ms = 5000;
    uint32_t duration_max_ms = 120000;
};

// =============================================================================
// SaturateCoordinatorActor — ramp state machine, rate broadcasting
// =============================================================================

class SaturateCoordinatorActor : public EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "SaturateCoordinatorActor";

    SaturateCoordinatorActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys),
          epoch_start_(std::chrono::steady_clock::now()) {
        build_presets();
        become(make_behavior());
    }

    void set_sender_addrs(std::vector<ActorAddress> addrs) {
        sender_addrs_ = std::move(addrs);
    }
    void set_receiver_addrs(std::vector<ActorAddress> addrs) {
        receiver_addrs_ = std::move(addrs);
    }
    void set_collector_addr(ActorAddress addr) {
        collector_addr_ = addr;
    }

    const std::vector<SaturatePreset>& presets() const {
        return presets_;
    }

    RampPhase current_phase() const {
        return phase_;
    }
    uint32_t current_rate_msgps() const {
        return current_rate_msgps_;
    }
    uint32_t saturation_ceiling_msgps() const {
        return saturation_ceiling_;
    }
    double current_drop_rate_pct() const {
        return current_drop_rate_pct_;
    }
    bool is_running() const {
        return running_;
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = kActorTypeName;
        m.state = phase_name(phase_);
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

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1, std::memory_order_relaxed);

            if (msg.type_id() == SaturateStartTag) {
                handle_start(msg);
            } else if (msg.type_id() == SaturateStopTag) {
                handle_stop();
            } else if (msg.type_id() == StatsPollTag) {
                handle_stats_poll();
            } else if (msg.type_id() == StatsReplyTag) {
                handle_stats_reply(msg);
            } else if (msg.type_id() == RampTickTag) {
                handle_ramp_tick();
            }
        }};
    }

  private:
    static constexpr TypeTag RampTickTag{0x00010211};

    void build_presets() {
        presets_.push_back(
            {"quick-saturate",
             "100 senders -> 10 receivers, small payload, fast ceiling find", 100,
             10, PayloadMode::Small, 0, 0, 100, 1000, 1.0f, 5, 4096, 5000, 30000});
        presets_.push_back(
            {"deep-saturate",
             "1000 senders -> 100 receivers, small payload, thorough curve",
             1000, 100, PayloadMode::Small, 0, 0, 100, 1000, 1.0f, 5, 8192,
             10000, 60000});
        presets_.push_back({"alloc-stress",
                            "500 senders -> 50 receivers, 1KB-64KB junk, allocator "
                            "pressure",
                            500, 50, PayloadMode::Junk, 1024, 65535, 50, 1000,
                            1.0f, 5, 2048, 5000, 60000});
        presets_.push_back(
            {"mixed-load",
             "500 senders -> 50 receivers, 80/20 mixed, realistic workload",
             500, 50, PayloadMode::Mixed, 16, 65535, 100, 1000, 1.0f, 5, 4096,
             5000, 60000});
        presets_.push_back(
            {"fan-in-extreme",
             "5000 senders -> 1 receiver, small payload, extreme contention", 5000,
             1, PayloadMode::Small, 0, 0, 10, 1000, 1.0f, 5, 16384, 5000, 60000});
        presets_.push_back({"fan-out-burst",
                            "10 senders -> 1000 receivers, 1KB-16KB junk, broad fan-out",
                            10, 1000, PayloadMode::Junk, 1024, 16384, 50, 1000,
                            1.0f, 5, 1024, 5000, 60000});
    }

    void handle_start(TypedMessage& msg) {
        const auto& p = msg.payload();
        std::string preset_name(p.data(), p.data() + p.size());

        if (sender_addrs_.empty() && receiver_addrs_.empty()) {
            last_error_ = "Actors not yet initialized. Please wait and try again.";
            return;
        }

        const SaturatePreset* preset = nullptr;
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

        active_senders_ = std::min(preset->num_senders,
                                   static_cast<uint32_t>(sender_addrs_.size()));
        active_receivers_ = std::min(
            preset->num_receivers, static_cast<uint32_t>(receiver_addrs_.size()));

        active_preset_ = preset->name;
        last_error_.clear();
        phase_ = RampPhase::Probing;
        current_rate_msgps_ = preset->initial_rate_msgps;
        actual_throughput_msgps_ = 0.0;
        saturation_ceiling_ = 0;
        refine_iteration_ = 0;
        last_good_rate_ = 0;
        first_bad_rate_ = 0;

        SaturateStartPayload start;
        start.num_senders = active_senders_;
        start.num_receivers = active_receivers_;
        start.payload_mode = preset->payload_mode;
        start.payload_size_min = preset->payload_size_min;
        start.payload_size_max = preset->payload_size_max;
        start.initial_rate_msgps = preset->initial_rate_msgps;
        start.step_interval_ms = preset->step_interval_ms;
        start.drop_threshold_pct = preset->drop_threshold_pct;
        start.refine_iterations = preset->refine_iterations;
        start.mailbox_capacity = preset->mailbox_capacity;
        start.stable_duration_ms = preset->stable_duration_ms;
        start.duration_max_ms = preset->duration_max_ms;

        auto start_buf = start.encode();
        for (uint32_t i = 0; i < active_senders_; ++i)
            context()->send(sender_addrs_[i], make_msg(SaturateStartTag, start_buf));
        for (uint32_t i = 0; i < active_receivers_; ++i)
            context()->send(receiver_addrs_[i],
                            make_msg(SaturateStartTag, start_buf));
        context()->send(collector_addr_, make_msg(SaturateStartTag, start_buf));

        broadcast_rate();

        running_ = true;
        start_time_ = std::chrono::steady_clock::now();
        schedule_ramp_tick();
    }

    void handle_stop() {
        StreamBuffer empty;
        for (uint32_t i = 0; i < active_senders_; ++i)
            context()->send(sender_addrs_[i], make_msg(SaturateStopTag, empty));
        for (uint32_t i = 0; i < active_receivers_; ++i)
            context()->send(receiver_addrs_[i], make_msg(SaturateStopTag, empty));
        context()->send(collector_addr_, make_msg(SaturateStopTag, empty));
        running_ = false;
        phase_ = RampPhase::Reporting;
    }

    void handle_stats_poll() {
        StreamBuffer empty;
        context()->send(collector_addr_, make_msg(StatsPollTag, empty));
    }

    void handle_stats_reply(TypedMessage& msg) {
        const auto& p = msg.payload();
        std::string report(p.data(), p.data() + p.size());
        std::istringstream iss(report);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.starts_with("drop_rate_pct=")) {
                current_drop_rate_pct_ = std::stod(line.substr(14));
            } else if (line.starts_with("throughput_msgps=")) {
                actual_throughput_msgps_ = std::stod(line.substr(18));
            }
        }
    }

    void handle_ramp_tick() {
        if (!running_)
            return;

        StreamBuffer empty;
        context()->send(collector_addr_, make_msg(StatsPollTag, empty));

        switch (phase_) {
            case RampPhase::Probing: {
                if (current_drop_rate_pct_ > 1.0f ||
                    current_rate_msgps_ >= kMaxRateMsgps) {
                    first_bad_rate_ = current_rate_msgps_;
                    last_good_rate_ = current_rate_msgps_ / 2;
                    phase_ = RampPhase::Refining;
                    refine_iteration_ = 0;
                } else {
                    last_good_rate_ = current_rate_msgps_;
                    current_rate_msgps_ =
                        std::min(current_rate_msgps_ * 2, kMaxRateMsgps);
                    broadcast_rate();
                }
                break;
            }
            case RampPhase::Refining: {
                if (refine_iteration_ >= 5) {
                    saturation_ceiling_ = last_good_rate_;
                    phase_ = RampPhase::Stable;
                    stable_start_ = std::chrono::steady_clock::now();
                    current_rate_msgps_ = saturation_ceiling_;
                    broadcast_rate();
                    break;
                }
                uint32_t mid = (last_good_rate_ + first_bad_rate_) / 2;
                if (current_drop_rate_pct_ > 1.0f) {
                    first_bad_rate_ = mid;
                } else {
                    last_good_rate_ = mid;
                }
                current_rate_msgps_ = mid;
                broadcast_rate();
                refine_iteration_++;
                break;
            }
            case RampPhase::Stable: {
                auto stable_elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - stable_start_)
                        .count();
                if (stable_elapsed >= 5000) {
                    phase_ = RampPhase::Reporting;
                    handle_stop();
                    return;
                }
                break;
            }
            default:
                break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start_time_)
                           .count();
        if (elapsed >= 120000) {
            handle_stop();
            return;
        }

        schedule_ramp_tick();
    }

    void broadcast_rate() {
        RateChangePayload rc;
        rc.target_rate_msgps = current_rate_msgps_;
        rc.step_interval_ms = 1000;
        auto buf = rc.encode();
        for (uint32_t i = 0; i < active_senders_; ++i)
            context()->send(sender_addrs_[i], make_msg(RateChangeTag, buf));
    }

    void schedule_ramp_tick() {
        context()->schedule(std::chrono::milliseconds(1000), make_msg(RampTickTag));
    }

    std::vector<uint8_t> build_report_bytes() const {
        std::ostringstream oss;
        oss << "preset=" << active_preset_ << "\n";
        oss << "phase=" << phase_name(phase_) << "\n";
        oss << "running=" << (running_ ? "yes" : "no") << "\n";
        if (!last_error_.empty())
            oss << "error=" << last_error_ << "\n";
        if (running_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start_time_)
                               .count();
            oss << "elapsed_ms=" << elapsed << "\n";
        }
        oss << "current_rate_msgps=" << current_rate_msgps_ << "\n";
        oss << "actual_throughput_msgps=" << actual_throughput_msgps_ << "\n";
        oss << "drop_rate_pct=" << current_drop_rate_pct_ << "\n";
        oss << "saturation_ceiling=" << saturation_ceiling_ << "\n";
        oss << "senders=" << active_senders_ << "\n";
        oss << "receivers=" << active_receivers_ << "\n";
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
    std::chrono::steady_clock::time_point stable_start_;
    std::vector<SaturatePreset> presets_;
    std::vector<ActorAddress> sender_addrs_;
    std::vector<ActorAddress> receiver_addrs_;
    uint32_t active_senders_ = 0;
    uint32_t active_receivers_ = 0;
    ActorAddress collector_addr_;
    std::string active_preset_;
    std::string last_error_;
    static constexpr uint32_t kMaxRateMsgps = 100'000'000; // 100M msg/s cap

    RampPhase phase_ = RampPhase::Idle;
    uint32_t current_rate_msgps_ = 100;
    double actual_throughput_msgps_ = 0.0;
    uint32_t saturation_ceiling_ = 0;
    uint32_t last_good_rate_ = 0;
    uint32_t first_bad_rate_ = 0;
    uint8_t refine_iteration_ = 0;
    double current_drop_rate_pct_ = 0.0;
    std::atomic<uint64_t> processed_{0};
    bool running_ = false;
};

} // namespace hpactor::apps::bench_saturate
