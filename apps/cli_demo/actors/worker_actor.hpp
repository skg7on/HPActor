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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/circuit_breaker.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/mailbox/actor_rate_limiter.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>

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

// =============================================================================
// Thread-safe random number generator
// =============================================================================
// rand() uses shared global state that is NOT thread-safe. Each thread needs
// its own generator instance.

namespace {

class Trng {
  public:
    Trng() {
        static std::atomic<uint64_t> s_counter{1};
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        state_ = static_cast<uint64_t>(now) ^ (s_counter.fetch_add(1) << 33);
        if (state_ == 0)
            state_ = 1;
    }

    uint64_t next() {
        uint64_t x = state_;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state_ = x;
        return x;
    }

    double next_double() {
        return static_cast<double>(next() >> 11) * 0x1.0p-53;
    }

  private:
    uint64_t state_;
};

thread_local Trng tl_rng;

} // namespace

// =============================================================================
// Worker configuration
// =============================================================================

/// \brief Per-worker feature flags for demonstrating different CLI commands.
struct WorkerConfig {
    uint32_t worker_id = 0;
    ActorId aggregator_id{0};
    ActorId log_id{0};

    /// \brief Rate limiter rate (msg/s). 0 = disabled.
    double rate_limit = 0.0;

    /// \brief Rate limiter burst allowance.
    uint32_t rate_burst = 10;

    /// \brief Enable quarantine/circuit breaker.
    bool quarantine_enabled = false;

    /// \brief Periodically send to nonexistent actor to generate failures.
    bool generate_delivery_failures = false;
};

// =============================================================================
// WorkerActor
// =============================================================================

/// \brief Periodic task processing actor with configurable production features.
///
/// Each worker instance can be independently configured with:
/// - Rate limiting (token bucket) — demonstrated via \c /actor &lt;id&gt; rate
/// - Circuit breaker + quarantine — demonstrated via \c /actor &lt;id&gt;
/// circuit
/// - Delivery failure generation — feeds \c /actor &lt;id&gt; delivery
/// - FAULT_INJECT sites — visible via \c /fault list
///
/// Workers send results to the AggregatorActor and log milestones to LogActor.
class WorkerActor : public EventBasedActor {
  public:
    WorkerActor(ActorContext* ctx, ActorSystem& sys, WorkerConfig cfg)
        : EventBasedActor(ctx, sys), cfg_(cfg),
          epoch_start_(std::chrono::steady_clock::now()),
          rate_limiter_configured_(false) {
        if (cfg.quarantine_enabled) {
            QuarantinePolicy qp;
            qp.enabled = true;
            qp.failure_rate_threshold = 5;        // trip after 5 failures/sec
            qp.timeout_rate_threshold = 3;        // trip after 3 timeouts/sec
            qp.mailbox_pressure_threshold = 0.8f; // trip at 80% pressure
            qp.cooldown_period = std::chrono::milliseconds{10'000};
            qp.observation_window = std::chrono::milliseconds{5'000};
            configure_quarantine(qp);
        }
        become(make_behavior());
    }

    /// \brief Override to configure rate limiter once the mailbox is available.
    void set_mailbox(mailbox::MPSCActorMailbox<TypedMessage>* mbox) override {
        EventBasedActor::set_mailbox(mbox);
        if (mbox && !rate_limiter_configured_ && cfg_.rate_limit > 0.0) {
            auto limiter = std::make_unique<mailbox::ActorRateLimiter>();
            limiter->configure(cfg_.rate_limit, cfg_.rate_burst);
            mbox->set_rate_limiter(std::move(limiter));
            rate_limiter_configured_ = true;
        }
    }

    void set_aggregator_addr(ActorAddress a) {
        aggregator_addr_ = a;
    }
    void set_log_addr(ActorAddress a) {
        log_addr_ = a;
    }

    // Expose rate limiter configuration for main() to set on the mailbox.
    double rate_limit() const {
        return cfg_.rate_limit;
    }
    uint32_t rate_burst() const {
        return cfg_.rate_burst;
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "WorkerActor";
        m.state = healthy_ ? "Running" : "Unhealthy";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        std::ostringstream oss;
        oss << "worker-" << cfg_.worker_id;
        m.behavior_name = oss.str();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "worker_id=" << cfg_.worker_id
            << " tasks_processed=" << tasks_processed_
            << " avg_latency_us=" << std::fixed << std::setprecision(1)
            << avg_latency_us_ << " current_load=" << std::fixed
            << std::setprecision(2) << current_load_
            << " healthy=" << (healthy_ ? "yes" : "no")
            << " delivery_accepted=" << delivery_accepted_.load()
            << " delivery_rejected=" << delivery_rejected_.load();

        // Rate limiter status
        if (cfg_.rate_limit > 0.0) {
            oss << "\n  rate_limiter: rate=" << cfg_.rate_limit
                << " msg/s burst=" << cfg_.rate_burst;
        } else {
            oss << "\n  rate_limiter: disabled";
        }

        // Circuit breaker status (const_cast safe: only reading state)
        if (auto* cb = const_cast<WorkerActor*>(this)->circuit_breaker()) {
            oss << "\n  circuit_breaker: state=" << to_string(cb->state)
                << " trips=" << cb->trip_count << " failure_ema=" << std::fixed
                << std::setprecision(3) << cb->failure_ema;
        }

        auto s = oss.str();
        return {s.begin(), s.end()};
    }

    uint32_t worker_id() const {
        return cfg_.worker_id;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == StartTag || msg.type_id() == PeriodicTickTag) {
                do_work();
            } else if (msg.type_id() == HealthPingTag) {
                auto load = static_cast<uint64_t>(current_load_ * 1000.0);
                context()->reply(make_msg(HealthPongTag, encode_u64(load)));
            }
            // BroadcastConfigTag — config update, no reply needed
        }};
    }

  private:
    void do_work() {
        // FAULT_INJECT: simulate enqueue failure for fault demo
        FAULT_INJECT("hpactor.app.worker.enqueue_fail") {
            // When this fault fires, skip the work iteration entirely to
            // simulate a dropped message. Visible via /fault status.
            context()->schedule(std::chrono::milliseconds(100),
                                make_msg(PeriodicTickTag));
            return;
        }

        // Thread-safe RNG
        double latency = 50.0 + static_cast<double>(tl_rng.next() % 450);

        tasks_processed_++;
        avg_latency_us_ = (avg_latency_us_ * 0.9) + (latency * 0.1);
        current_load_ = 0.3 + tl_rng.next_double() * 0.7;

        // Worker-3: occasionally inject anomalous behavior to trip circuit
        // breaker
        bool success = true;
        if (cfg_.quarantine_enabled && tasks_processed_ % 15 == 0) {
            // Simulate a high-latency anomaly (~20% failure injection)
            if (tl_rng.next() % 5 == 0) {
                latency = 5000.0; // 5ms — abnormally high
                success = false;
                // Also simulate a brief unhealthy state
                healthy_ = false;
            } else {
                healthy_ = true;
            }
        }

        if (cfg_.quarantine_enabled) {
            record_circuit_breaker_result(success);
            if (!success) {
                record_circuit_breaker_timeout();
            }
        }

        // Send result to Aggregator
        StreamBuffer payload(16);
        auto* data = reinterpret_cast<double*>(payload.data());
        data[0] = latency;
        double throughput = 1000.0 + static_cast<double>(tl_rng.next() % 4000);
        data[1] = throughput;
        delivery_accepted_.fetch_add(1);
        context()->send(aggregator_addr_,
                        make_msg(WorkerResultTag, std::move(payload)));

        // Log milestone events to LogActor
        if (tasks_processed_ % 100 == 0) {
            char buf[128];
            int n = snprintf(buf, sizeof(buf),
                             "[Worker-%u] %lu tasks, avg %.0f us", cfg_.worker_id,
                             static_cast<unsigned long>(tasks_processed_),
                             avg_latency_us_);
            StreamBuffer log_payload(reinterpret_cast<const uint8_t*>(buf),
                                     reinterpret_cast<const uint8_t*>(buf + n));
            context()->send(log_addr_,
                            make_msg(LogEntryTag, std::move(log_payload)));
        }

        // Worker-4: periodically send to nonexistent actor to generate
        // delivery failures visible via /actor <id> delivery
        if (cfg_.generate_delivery_failures && tasks_processed_ % 10 == 0) {
            // Send to a nonexistent ActorId — generates delivery failure
            ActorId dead_id{0xDEAD};
            StreamBuffer junk(8);
            auto* junk_data = reinterpret_cast<uint64_t*>(junk.data());
            junk_data[0] = tasks_processed_;
            system().deliver_local(dead_id,
                                   make_msg(PeriodicTickTag, std::move(junk)));
            delivery_rejected_.fetch_add(1);
        }

        // Schedule next tick
        context()->schedule(std::chrono::milliseconds(100),
                            make_msg(PeriodicTickTag));
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    WorkerConfig cfg_;
    ActorAddress aggregator_addr_;
    ActorAddress log_addr_;
    std::chrono::steady_clock::time_point epoch_start_;
    bool rate_limiter_configured_ = false;
    uint64_t tasks_processed_ = 0;
    double avg_latency_us_ = 0.0;
    double current_load_ = 0.0;
    bool healthy_ = true;
    std::atomic<uint64_t> processed_{0};
    std::atomic<uint64_t> delivery_accepted_{0};
    std::atomic<uint64_t> delivery_rejected_{0};
};

} // namespace hpactor::apps::cli_demo
