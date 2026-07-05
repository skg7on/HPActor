// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
#pragma once

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>

#include "../messages.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace hpactor::apps::bench_caf {

inline constexpr TypeTag PingTag{0x0001030C};
inline constexpr TypeTag PongTag{0x0001030D};

struct DistributedPingCounters {
    std::atomic<uint64_t> pings_sent{0};
    std::atomic<uint64_t> pongs_received{0};
};

class PingActor : public EventBasedActor {
  public:
    PingActor(ActorContext* ctx, ActorSystem& sys,
              DistributedPingCounters* counters, std::vector<ActorAddress> targets,
              uint32_t pings_per_target, uint64_t seed)
        : EventBasedActor(ctx, sys), counters_(counters),
          targets_(std::move(targets)), pings_per_target_(pings_per_target),
          seed_(seed) {
        become(make_behavior());
    }

    void set_targets(std::vector<ActorAddress> targets) {
        targets_ = std::move(targets);
    }
    void set_pings_per_target(uint32_t n) {
        pings_per_target_ = n;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == PingTag) {
                // Reply with pong to sender.
                context()->send(msg.sender_address(), make_bench_msg(PongTag));
            } else if (msg.type_id() == PongTag) {
                counters_->pongs_received.fetch_add(1, std::memory_order_relaxed);
            } else if (msg.type_id() == MailboxLoadTag) {
                // Start sending pings.
                for (const auto& target : targets_) {
                    for (uint32_t i = 0; i < pings_per_target_; ++i) {
                        BenchPayloadHeader header;
                        header.sender_id = static_cast<uint32_t>(seed_);
                        header.sequence = i;
                        context()->send(
                            target,
                            make_bench_msg(PingTag, encode_bench_payload(
                                                        header, 0, seed_ + i)));
                        counters_->pings_sent.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        }};
    }

  private:
    DistributedPingCounters* counters_ = nullptr;
    std::vector<ActorAddress> targets_;
    uint32_t pings_per_target_ = 0;
    uint64_t seed_ = 0;
};

} // namespace hpactor::apps::bench_caf
