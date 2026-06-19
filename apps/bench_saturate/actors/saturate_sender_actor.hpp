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
// SaturateSenderActor — rate-directed load generator (cooperative loop)
//
// Instead of timer-driven per-batch scheduling, the sender uses the
// scheduler's built-in RequeueReady mechanism for continuous operation.
// After each batch, the handler returns and the scheduler re-invokes
// it up to kRequeueBudget=64 times.  When ahead of the target rate,
// a short-duration timer yields the CPU.  When behind, the sender runs
// continuously until the requeue budget is exhausted, at which point
// the next enqueued message triggers a new activation cycle via
// notify_ready.
// =============================================================================

class SaturateSenderActor : public EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "SaturateSenderActor";

    SaturateSenderActor(ActorContext* ctx, ActorSystem& sys,
                        ActorAddress collector_addr,
                        const std::vector<ActorAddress>& receiver_addrs,
                        uint32_t sender_index)
        : EventBasedActor(ctx, sys), collector_addr_(collector_addr),
          receiver_addrs_(receiver_addrs), sender_index_(sender_index),
          epoch_start_(std::chrono::steady_clock::now()) {
        become(make_behavior());
    }

    void set_receiver_addrs(const std::vector<ActorAddress>& addrs) {
        receiver_addrs_ = addrs;
    }

    uint64_t sent_count() const {
        return sent_count_.load();
    }
    uint32_t sender_index() const {
        return sender_index_;
    }
    uint32_t current_rate_msgps() const {
        return current_rate_msgps_;
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
        oss << "saturate-sender-" << sender_index_
            << " rate=" << current_rate_msgps_ << "msg/s";
        m.behavior_name = oss.str();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "sender_index=" << sender_index_
            << " sent=" << sent_count_.load() << " rate=" << current_rate_msgps_
            << " running=" << (running_ ? "yes" : "no");
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1, std::memory_order_relaxed);

            if (msg.type_id() == RateChangeTag) {
                handle_rate_change(msg);
            } else if (msg.type_id() == SaturateStartTag) {
                handle_start(msg);
            } else if (msg.type_id() == SaturateStopTag) {
                running_ = false;
            } else if (msg.type_id() == SendTickTag) {
                if (running_)
                    do_tick();
            }
        }};
    }

  private:
    static constexpr TypeTag SendTickTag{0x00010210};
    static constexpr uint32_t kBatchSize = 10;

    void handle_rate_change(TypedMessage& msg) {
        auto rc = RateChangePayload::decode(msg.payload());
        current_rate_msgps_ = rc.target_rate_msgps;
        payload_mode_ = rc.payload_mode;
        payload_size_min_ = rc.payload_size_min;
        payload_size_max_ = rc.payload_size_max;
        step_interval_ms_ = rc.step_interval_ms;
        // No explicit schedule — do_tick() handles its own pacing via
        // the scheduler's requeue budget.
    }

    void handle_start(TypedMessage& msg) {
        sent_count_.store(0);
        send_dropped_.store(0);
        seq_no_ = 0;
        next_receiver_idx_ = 0;

        // Honour the preset's receiver count so the sender round-robins
        // only over the active subset, not all pre-created receivers.
        auto start = SaturateStartPayload::decode(msg.payload());
        num_active_receivers_ = start.num_receivers;
        if (num_active_receivers_ == 0 ||
            num_active_receivers_ > receiver_addrs_.size())
            num_active_receivers_ = static_cast<uint32_t>(receiver_addrs_.size());

        running_ = true;
        start_time_ = std::chrono::steady_clock::now();
        // Kick off the send loop via zero-delay self-message so the first
        // batch runs from the scheduler (not a recursive call from start).
        pending_tick_ = context()->schedule(std::chrono::milliseconds(0),
                                            make_msg(SendTickTag));
    }

    void do_tick() {
        if (!running_ || receiver_addrs_.empty())
            return;

        auto now = std::chrono::steady_clock::now();

        // ── Rate throttling ──────────────────────────────────────────
        // If we're significantly ahead of the target rate, yield via a
        // short timer rather than spinning.  The scheduler will re-invoke
        // us when the timer fires.
        uint64_t sent = sent_count_.load(std::memory_order_relaxed);
        uint64_t elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - start_time_)
                .count());
        if (elapsed_us > 0 && current_rate_msgps_ > 0) {
            uint64_t expected_us = sent * 1'000'000ULL / current_rate_msgps_;
            if (expected_us > elapsed_us + 500) {
                // Ahead of schedule — sleep until we catch up.
                uint64_t sleep_us = expected_us - elapsed_us;
                if (sleep_us > 10000)
                    sleep_us = 10000; // cap at 10ms
                auto sleep_ms = std::chrono::milliseconds(
                    std::max<uint64_t>(1, sleep_us / 1000));
                pending_tick_ =
                    context()->schedule(sleep_ms, make_msg(SendTickTag));
                return;
            }
        }

        // ── Send batch ───────────────────────────────────────────────
        uint64_t now_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch())
                .count());

        for (uint32_t i = 0; i < kBatchSize; ++i) {
            PayloadMode mode = payload_mode_;
            if (mode == PayloadMode::Mixed) {
                mode = (seq_no_ % 5 == 0) ? PayloadMode::Junk : PayloadMode::Small;
            }

            LoadMessagePayload load;
            load.sender_id = sender_index_;
            load.seq_no = seq_no_++;
            load.send_timestamp_us = now_us;

            StreamBuffer payload;
            if (mode == PayloadMode::Small) {
                payload = load.encode_header();
            } else {
                size_t junk_size = random_payload_size(
                    payload_size_min_, payload_size_max_, seq_no_seed_);
                payload = load.encode_with_junk(junk_size, seq_no_seed_);
            }

            auto& target = receiver_addrs_[next_receiver_idx_];
            next_receiver_idx_ = static_cast<uint32_t>((next_receiver_idx_ + 1) %
                                                       num_active_receivers_);

            // Use the fast delivery path — no pipeline overhead.
            auto result = home_system().try_deliver_local_fast(
                target.id, make_msg(LoadMessageTag, std::move(payload)));
            sent_count_.fetch_add(1, std::memory_order_relaxed);
            if (!result.accepted())
                send_dropped_.fetch_add(1, std::memory_order_relaxed);
        }

        // ── Periodic throughput sampling ─────────────────────────────
        sent = sent_count_.load(std::memory_order_relaxed);
        if (sent % 100 == 0) {
            ThroughputSamplePayload tsp;
            tsp.sender_id = sender_index_;
            tsp.total_sent = sent;
            tsp.send_dropped = send_dropped_.load();
            context()->send(collector_addr_,
                            make_msg(ThroughputSampleTag, tsp.encode()));
        }

        // Schedule the next tick.  When not rate-throttling, use a
        // zero-delay timer that fires on the next scheduler iteration
        // (equivalent to the RequeueReady path but works even when the
        // actor's own mailbox is empty after consuming the current tick).
        pending_tick_ = context()->schedule(std::chrono::milliseconds(0),
                                            make_msg(SendTickTag));
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    ActorAddress collector_addr_;
    std::vector<ActorAddress> receiver_addrs_;
    uint32_t sender_index_;
    uint32_t num_active_receivers_ = 0;
    AlarmHandle pending_tick_;
    std::chrono::steady_clock::time_point epoch_start_;
    std::chrono::steady_clock::time_point start_time_;
    std::atomic<uint64_t> sent_count_{0};
    std::atomic<uint64_t> send_dropped_{0};
    std::atomic<uint64_t> processed_{0};
    uint64_t seq_no_ = 0;
    uint64_t seq_no_seed_ = 12345;
    uint32_t current_rate_msgps_ = 100;
    uint32_t next_receiver_idx_ = 0;
    PayloadMode payload_mode_ = PayloadMode::Small;
    uint16_t payload_size_min_ = 16;
    uint16_t payload_size_max_ = 16;
    uint16_t step_interval_ms_ = 1000;
    bool running_ = false;
};

} // namespace hpactor::apps::bench_saturate
