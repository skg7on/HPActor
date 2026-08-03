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
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>

#include "../messages.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::cli_demo {

// Message tags for MailboxLoadActor (must precede class — used in Behavior)
inline constexpr TypeTag BurstTag{0x00010010};
inline constexpr TypeTag DrainTag{0x00010011};
inline constexpr TypeTag SteadyTag{0x00010012};

// =============================================================================
// MailboxLoadActor
// =============================================================================
//
// An actor that deliberately fills its own mailbox to demonstrate mailbox
// observation features added in MBX-007:
//
//   - Pressure state transitions: Normal → SoftPressure → HardPressure
//   - Mailbox capacity vs. depth gauges
//   - Queued bytes tracking
//   - Max depth tracking
//   - System lane depth (system messages insert into protected lane)
//
// Modes (cycle every ~10s):
//   1. BURST  — schedule 200 self-messages rapidly (fills mailbox past
//   capacity)
//   2. DRAIN  — process backlog, pressure recovers
//   3. STEADY — moderate load, depth hovers near 50%
//
// Observable via:
//   /actor <id> show       — full mailbox snapshot (depth, capacity, pressure)
//   /metrics show          — Prometheus gauges: hpactor_mailbox_*
//   /actor <id> delivery   — delivery result counters
//
class MailboxLoadActor : public EventBasedActor {
  public:
    MailboxLoadActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys),
          epoch_start_(std::chrono::steady_clock::now()) {
        become(make_behavior());
    }

    /// \brief Override mailbox config for visible pressure transitions.
    ///
    /// Sets a small 64-message capacity with RejectNewest overflow so that
    /// Burst mode fills the mailbox quickly and triggers pressure gauge
    /// transitions observable via /metrics show.
    void set_mailbox(mailbox::MPSCActorMailbox<TypedMessage>* mbox) override {
        EventBasedActor::set_mailbox(mbox);
        if (mbox && !mailbox_configured_) {
            auto cfg = mbox->config();
            cfg.capacity.max_messages = 64;
            cfg.overflow_policy = mailbox::OverflowPolicy::RejectNewest;
            mbox->set_config(cfg);
            mailbox_configured_ = true;
        }
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "MailboxLoadActor";
        m.state = mode_label();
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        m.behavior_name = "mailbox-load";
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "mode=" << mode_label() << " burst_count=" << burst_count_
            << " messages_processed=" << processed_.load()
            << "\n  Use /metrics show to see hpactor_mailbox_* gauges for this "
               "actor.";
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);

            if (msg.type_id() == StartTag) {
                kick_mode();
                schedule_next();
            } else if (msg.type_id() == BurstTag) {
                // BURST mode: enqueue many self-messages to fill mailbox.
                // Each processed BurstTag spawns up to 20 more, creating a
                // rapid fan-out that fills the mailbox past capacity and
                // triggers HardPressure.
                burst_count_++;
                if (burst_count_ < burst_target_) {
                    for (int i = 0; i < 20 && burst_count_ < burst_target_; ++i) {
                        self_send(BurstTag);
                        burst_count_++;
                    }
                }
            } else if (msg.type_id() == SteadyTag) {
                // STEADY mode: maintain moderate load — enqueue 2 more
                // for each processed, keeping depth near 50%.
                for (int i = 0; i < 2; ++i) {
                    self_send(SteadyTag);
                }
            } else if (msg.type_id() == PeriodicTickTag) {
                // Mode cycle timer — rotate every ~10s
                cycle_mode();
            }
            // DrainTag: fall through — just process, no re-enqueue.
            // Mailbox naturally drains as we consume messages.
        }};
    }

  private:
    enum class Mode : uint8_t { Burst, Drain, Steady };

    const char* mode_label() const {
        switch (mode_) {
            case Mode::Burst:
                return "Burst";
            case Mode::Drain:
                return "Drain";
            case Mode::Steady:
                return "Steady";
        }
        return "Unknown";
    }

    void cycle_mode() {
        switch (mode_) {
            case Mode::Burst:
                mode_ = Mode::Drain;
                break;
            case Mode::Drain:
                mode_ = Mode::Steady;
                break;
            case Mode::Steady:
                mode_ = Mode::Burst;
                burst_count_ = 0;
                break;
        }
        kick_mode();
    }

    void kick_mode() {
        switch (mode_) {
            case Mode::Burst:
                // Rapid burst of self-messages to exceed capacity
                burst_count_ = 0;
                for (int i = 0; i < 30; ++i) {
                    self_send(BurstTag);
                    burst_count_++;
                }
                break;
            case Mode::Drain:
                // Natural drain — just process what's queued
                self_send(DrainTag);
                break;
            case Mode::Steady:
                self_send(SteadyTag);
                break;
        }
    }

    void self_send(TypeTag tag) {
        context()->schedule(std::chrono::milliseconds(0), make_msg(tag));
    }

    void schedule_next() {
        // Schedule a mode-cycle check in ~10 seconds
        context()->schedule(std::chrono::seconds(10), make_msg(PeriodicTickTag));
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    std::chrono::steady_clock::time_point epoch_start_;
    std::atomic<uint64_t> processed_{0};
    Mode mode_{Mode::Burst};
    uint32_t burst_count_{0};
    bool mailbox_configured_{false};
    static constexpr uint32_t burst_target_{200};
};

} // namespace hpactor::apps::cli_demo
