// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/cli/cli_types.hpp>

#include "../messages.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor::apps::cli_demo {

/// \brief Periodically sends request-response queries to ClockActor.
///
/// Every 2 seconds sends a TimeQueryTag request to ClockActor, which
/// replies with TimeReplyTag via context()->reply(). Tracks sent/received
/// counts and average latency. Provides real data for /ask pending,
/// /ask cancel, and /ask stats CLI commands.
class QueryActor : public EventBasedActor {
  public:
    QueryActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys),
          epoch_start_(std::chrono::steady_clock::now()) {
        become(make_behavior());
    }

    void set_clock_addr(ActorAddress addr) {
        clock_addr_ = addr;
    }

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = "QueryActor";
        m.state = "Running";
        m.messages_processed = processed_.load();
        m.uptime_ms = elapsed_ms();
        return m;
    }

    std::vector<uint8_t> serialize_state() const override {
        std::ostringstream oss;
        oss << "queries_sent=" << queries_sent_
            << " responses_received=" << responses_received_
            << " timeouts=" << timeouts_ << " avg_latency_us=" << avg_latency_us_;
        auto s = oss.str();
        return {s.begin(), s.end()};
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            processed_.fetch_add(1);
            if (msg.type_id() == StartTag || msg.type_id() == QueryTriggerTag) {
                do_query();
            } else if (msg.type_id() == TimeReplyTag) {
                uint64_t clock_time = decode_u64(msg.payload());
                uint64_t now_us = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - epoch_start_)
                        .count());
                uint64_t latency = now_us - last_query_sent_us_;
                avg_latency_us_ = (avg_latency_us_ * 0.9) +
                                  (static_cast<double>(latency) * 0.1);
                responses_received_++;
                (void)clock_time;
            }
        }};
    }

  private:
    void do_query() {
        if (clock_addr_) {
            last_query_sent_us_ = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - epoch_start_)
                    .count());
            queries_sent_++;

            // Send a request to ClockActor; ClockActor will reply via
            // context()->reply() with TimeReplyTag, which is handled above.
            context()->send(clock_addr_, make_msg(TimeQueryTag));
        }

        context()->schedule(std::chrono::milliseconds(2000),
                            make_msg(QueryTriggerTag));
    }

    uint64_t elapsed_ms() const {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - epoch_start_)
                .count());
    }

    ActorAddress clock_addr_;
    std::chrono::steady_clock::time_point epoch_start_;
    uint64_t queries_sent_ = 0;
    uint64_t responses_received_ = 0;
    uint64_t timeouts_ = 0;
    uint64_t last_query_sent_us_ = 0;
    double avg_latency_us_ = 0.0;
    std::atomic<uint64_t> processed_{0};
};

} // namespace hpactor::apps::cli_demo
