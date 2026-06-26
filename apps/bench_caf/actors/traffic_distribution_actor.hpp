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
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>

#include "../caf_bench_config.hpp"
#include "../messages.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace hpactor::apps::bench_caf {

// ── Shared distribution counters ──────────────────────────────

struct DistributionCounters {
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> senders_done{0};
    std::atomic<uint64_t> receivers_done{0};
};

// ── One-to-One: single sender, single receiver ────────────────

class OneToOneSender : public EventBasedActor {
  public:
    OneToOneSender(ActorContext* ctx, ActorSystem& sys,
                   DistributionCounters* counters, ActorAddress receiver,
                   uint32_t messages, uint32_t payload_size, uint64_t seed)
        : EventBasedActor(ctx, sys), counters_(counters), receiver_(receiver),
          messages_(messages), payload_size_(payload_size), seed_(seed) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != MailboxLoadTag)
                return;
            for (uint32_t i = 0; i < messages_; ++i) {
                BenchPayloadHeader header;
                header.sender_id = 0;
                header.sequence = i;
                auto payload =
                    encode_bench_payload(header, payload_size_, seed_ + i);
                context()->send(receiver_, make_bench_msg(MailboxLoadTag,
                                                          std::move(payload)));
                counters_->sent.fetch_add(1, std::memory_order_relaxed);
            }
            counters_->senders_done.fetch_add(1, std::memory_order_release);
        }};
    }

  private:
    DistributionCounters* counters_ = nullptr;
    ActorAddress receiver_;
    uint32_t messages_ = 0;
    uint32_t payload_size_ = 0;
    uint64_t seed_ = 0;
};

class OneToOneReceiver : public EventBasedActor {
  public:
    OneToOneReceiver(ActorContext* ctx, ActorSystem& sys,
                     DistributionCounters* counters)
        : EventBasedActor(ctx, sys), counters_(counters) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == MailboxLoadTag)
                counters_->received.fetch_add(1, std::memory_order_relaxed);
        }};
    }

  private:
    DistributionCounters* counters_ = nullptr;
};

// ── One-to-N: one sender, many receivers (round-robin) ────────

struct OneToNDimensions {
    uint32_t receivers = 8;
    uint32_t messages_per_receiver = 1000;
};

class OneToNSender : public EventBasedActor {
  public:
    OneToNSender(ActorContext* ctx, ActorSystem& sys, DistributionCounters* counters,
                 std::vector<ActorAddress> receivers, uint32_t messages_per,
                 uint32_t payload_size, uint64_t seed)
        : EventBasedActor(ctx, sys), counters_(counters),
          receivers_(std::move(receivers)), messages_per_(messages_per),
          payload_size_(payload_size), seed_(seed) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != MailboxLoadTag)
                return;
            uint64_t total = messages_per_ * receivers_.size();
            for (uint64_t i = 0; i < total; ++i) {
                BenchPayloadHeader header;
                header.sender_id = 0;
                header.sequence = i;
                auto payload =
                    encode_bench_payload(header, payload_size_, seed_ + i);
                uint32_t idx = static_cast<uint32_t>(i % receivers_.size());
                context()->send(receivers_[idx],
                                make_bench_msg(MailboxLoadTag, std::move(payload)));
                counters_->sent.fetch_add(1, std::memory_order_relaxed);
            }
            counters_->senders_done.fetch_add(1, std::memory_order_release);
        }};
    }

  private:
    DistributionCounters* counters_ = nullptr;
    std::vector<ActorAddress> receivers_;
    uint32_t messages_per_ = 0;
    uint32_t payload_size_ = 0;
    uint64_t seed_ = 0;
};

class OneToNReceiver : public EventBasedActor {
  public:
    OneToNReceiver(ActorContext* ctx, ActorSystem& sys, DistributionCounters* counters)
        : EventBasedActor(ctx, sys), counters_(counters) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == MailboxLoadTag)
                counters_->received.fetch_add(1, std::memory_order_relaxed);
        }};
    }

  private:
    DistributionCounters* counters_ = nullptr;
};

// ── N-to-N-Random: many senders, many receivers (LCG routing) ──

struct NToNRandomDimensions {
    uint32_t senders = 4;
    uint32_t receivers = 4;
    uint32_t messages_per_sender = 250;
};

class NToNRandomSender : public EventBasedActor {
  public:
    NToNRandomSender(ActorContext* ctx, ActorSystem& sys,
                     DistributionCounters* counters,
                     std::vector<ActorAddress> receivers, uint32_t sender_id,
                     uint32_t messages, uint32_t payload_size, uint64_t seed)
        : EventBasedActor(ctx, sys), counters_(counters),
          receivers_(std::move(receivers)), sender_id_(sender_id),
          messages_(messages), payload_size_(payload_size), seed_(seed) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != MailboxLoadTag)
                return;
            uint64_t lcg = seed_ + sender_id_;
            for (uint32_t i = 0; i < messages_; ++i) {
                BenchPayloadHeader header;
                header.sender_id = sender_id_;
                header.sequence = i;
                auto payload =
                    encode_bench_payload(header, payload_size_, seed_ + i);
                lcg = lcg * 6364136223846793005ULL + 1442695040888963407ULL;
                uint32_t idx = static_cast<uint32_t>(lcg % receivers_.size());
                context()->send(receivers_[idx],
                                make_bench_msg(MailboxLoadTag, std::move(payload)));
                counters_->sent.fetch_add(1, std::memory_order_relaxed);
            }
            counters_->senders_done.fetch_add(1, std::memory_order_release);
        }};
    }

  private:
    DistributionCounters* counters_ = nullptr;
    std::vector<ActorAddress> receivers_;
    uint32_t sender_id_ = 0;
    uint32_t messages_ = 0;
    uint32_t payload_size_ = 0;
    uint64_t seed_ = 0;
};

} // namespace hpactor::apps::bench_caf
