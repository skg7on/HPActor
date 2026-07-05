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

#include "../caf_bench_config.hpp"
#include "../messages.hpp"

#include <atomic>
#include <cstdint>

namespace hpactor::apps::bench_caf {

struct MailboxN1Counters {
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> senders_done{0};
};

struct MailboxN1Dimensions {
    uint32_t senders = 4;
    uint32_t messages_per_sender = 10000;
};

inline MailboxN1Dimensions mailbox_n1_dimensions_for_preset(PresetKind preset) {
    switch (preset) {
        case PresetKind::Smoke:
            return {4, 10000};
        case PresetKind::Nightly:
            return {32, 100000};
        case PresetKind::PaperScale:
            return {100, 1000000};
        case PresetKind::Stress:
            return {128, 1000000};
    }
    return {4, 10000};
}

class MailboxN1ReceiverActor : public EventBasedActor {
  public:
    MailboxN1ReceiverActor(ActorContext* ctx, ActorSystem& sys,
                           MailboxN1Counters* counters)
        : EventBasedActor(ctx, sys), counters_(counters) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == MailboxLoadTag) {
                counters_->received.fetch_add(1, std::memory_order_relaxed);
            }
        }};
    }

  private:
    MailboxN1Counters* counters_ = nullptr;
};

class MailboxN1SenderActor : public EventBasedActor {
  public:
    /// Messages sent per behavior invocation before yielding the worker thread.
    static constexpr uint32_t kBatchSize = 500;

    MailboxN1SenderActor(ActorContext* ctx, ActorSystem& sys,
                         MailboxN1Counters* counters, ActorAddress receiver,
                         uint32_t sender_id, uint32_t messages_to_send,
                         uint32_t payload_size, uint64_t seed)
        : EventBasedActor(ctx, sys), counters_(counters), receiver_(receiver),
          sender_id_(sender_id), messages_to_send_(messages_to_send),
          payload_size_(payload_size), seed_(seed) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != MailboxLoadTag)
                return;
            uint32_t end = std::min(sent_so_far_ + kBatchSize, messages_to_send_);
            for (uint32_t i = sent_so_far_; i < end; ++i) {
                BenchPayloadHeader header;
                header.sender_id = sender_id_;
                header.sequence = i;
                auto payload =
                    encode_bench_payload(header, payload_size_, seed_ + i);
                context()->send(receiver_, make_bench_msg(MailboxLoadTag,
                                                          std::move(payload)));
                counters_->sent.fetch_add(1, std::memory_order_relaxed);
            }
            sent_so_far_ = end;
            if (sent_so_far_ < messages_to_send_) {
                // Yield the worker thread so the receiver can drain.
                // Self-delivery re-enters this handler for the next batch.
                context()->send(this->address(), make_bench_msg(MailboxLoadTag));
            } else {
                counters_->senders_done.fetch_add(1, std::memory_order_release);
            }
        }};
    }

  private:
    MailboxN1Counters* counters_ = nullptr;
    ActorAddress receiver_;
    uint32_t sender_id_ = 0;
    uint32_t messages_to_send_ = 0;
    uint32_t sent_so_far_ = 0;
    uint32_t payload_size_ = 0;
    uint64_t seed_ = 0;
};

} // namespace hpactor::apps::bench_caf
