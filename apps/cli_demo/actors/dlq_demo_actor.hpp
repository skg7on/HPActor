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
#include <hpactor/actor/lifecycle/circuit_breaker.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/fault/fault_macros.hpp>

#include "../messages.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::cli_demo {

/// \brief Generates dead-letter queue records for CLI demonstration.
///
/// Periodically sends messages to nonexistent actors and intentionally
/// overflows bounded mailboxes, producing \c DeadLetterRecord entries
/// with varied reasons (ActorNotFound, MailboxFull, Expired).
///
/// Enabled features:
/// - Circuit breaker + quarantine — for \c /actor &lt;id&gt; circuit demo
/// - FAULT_INJECT sites — visible via \c /fault list
/// - Delivery failure counters — visible via \c /actor &lt;id&gt; delivery
///
/// DLQ records appear in \c /dlq list and can be inspected/replayed/exported.
class DlqDemoActor : public EventBasedActor {
  public:
    DlqDemoActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys),
          epoch_start_(std::chrono::steady_clock::now()) {
        // Enable quarantine + circuit breaker for CLI demo
        QuarantinePolicy qp;
        qp.enabled = true;
        qp.failure_rate_threshold = 3;
        qp.cooldown_period = std::chrono::milliseconds{15'000};
        qp.observation_window = std::chrono::milliseconds{5'000};
        configure_quarantine(qp);

        become(make_behavior());
    }

    void set_target_actors(const std::vector<ActorAddress>& targets) {
        targets_ = targets;
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "DlqDemoActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "dlq_records_generated=" << dlq_records_generated_
            << " not_found_sends=" << not_found_sends_
            << " expired_sends=" << expired_sends_
            << " overflow_sends=" << overflow_sends_ << " cycle=" << cycle_;

        // Circuit breaker status (const_cast safe: only reading state)
        if (auto* cb = const_cast<DlqDemoActor*>(this)->circuit_breaker()) {
            oss << "\n  circuit_breaker: state=" << to_string(cb->state)
                << " trips=" << cb->trip_count << " failure_ema=" << std::fixed
                << std::setprecision(3) << cb->failure_ema;
        }

        auto s = oss.str();
        return {s.begin(), s.end()};
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == StartTag || msg.type_id() == DlqGenerateTag) {
                generate_dlq_records();
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

    void generate_dlq_records() {
        // FAULT_INJECT: simulate DLQ path fault
        FAULT_INJECT("hpactor.app.dlq_demo.generate_fail") {
            // When this fault fires, skip generation. Track as a failure
            // for circuit breaker observability.
            record_circuit_breaker_result(false);
            context()->schedule(std::chrono::milliseconds(3000),
                                make_msg(DlqGenerateTag));
            return;
        }

        record_circuit_breaker_result(true);

        // 1. Send to nonexistent actor → ActorNotFound DLQ reason
        {
            ActorId ghost_id{0xBEEF};
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "dlq-demo:not-found:cycle-%lu",
                             static_cast<unsigned long>(cycle_));
            StreamBuffer payload(reinterpret_cast<const uint8_t*>(buf),
                                 reinterpret_cast<const uint8_t*>(buf + n));
            system().deliver_local(ghost_id,
                                   make_msg(LogEntryTag, std::move(payload)));
            not_found_sends_++;
        }

        // 2. Send message with a past deadline → Expired DLQ reason
        {
            char buf[64];
            int n = snprintf(buf, sizeof(buf), "dlq-demo:expired:cycle-%lu",
                             static_cast<unsigned long>(cycle_));
            StreamBuffer payload(reinterpret_cast<const uint8_t*>(buf),
                                 reinterpret_cast<const uint8_t*>(buf + n));
            // Set deadline to 1ns in the past — guaranteed expired
            system().deliver_local(ActorId{0xBEEF},
                                   make_msg(LogEntryTag, std::move(payload)), 0,
                                   /*deadline_ns=*/1);
            expired_sends_++;
        }

        // 3. Flood a target's mailbox with large payloads → MailboxFull
        // Send multiple large messages to a real target's bounded mailbox.
        // The 256-message capacity will eventually overflow into DLQ.
        if (!targets_.empty()) {
            // Pick target based on cycle
            auto& target = targets_[cycle_ % targets_.size()];
            for (uint32_t i = 0; i < 8; ++i) {
                // Create a large-ish payload (~512 bytes)
                std::vector<uint8_t> large_payload(512, 0xAB);
                char label[32];
                int n = snprintf(label, sizeof(label), "dlq-overflow:c%lu:b%d",
                                 static_cast<unsigned long>(cycle_), i);
                std::memcpy(large_payload.data(), label, static_cast<size_t>(n));
                StreamBuffer buf(large_payload.data(),
                                 large_payload.data() + large_payload.size());
                context()->send(target, make_msg(LogEntryTag, std::move(buf)));
                overflow_sends_++;
            }
        }

        dlq_records_generated_ += 3; // one batch of each type

        cycle_++;
        context()->schedule(std::chrono::milliseconds(3000),
                            make_msg(DlqGenerateTag));
    }

    std::vector<ActorAddress> targets_;
    uint64_t cycle_ = 0;
    uint64_t dlq_records_generated_ = 0;
    uint64_t not_found_sends_ = 0;
    uint64_t expired_sends_ = 0;
    uint64_t overflow_sends_ = 0;
    std::atomic<uint64_t> processed_{0};
    std::chrono::steady_clock::time_point epoch_start_;
};

} // namespace hpactor::apps::cli_demo
